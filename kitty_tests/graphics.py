#!/usr/bin/env python3
# vim:fileencoding=utf-8
# License: GPL v3 Copyright: 2016, Kovid Goyal <kovid at kovidgoyal.net>

import os
import shutil
import tempfile
import unittest
import zlib
from itertools import cycle
from base64 import standard_b64decode, standard_b64encode
from io import BytesIO

from kitty.constants import cache_dir
from kitty.fast_data_types import (
    load_png_data, parse_bytes, set_send_to_gpu, shm_unlink, shm_write,
    xor_data
)

from . import BaseTest

try:
    from PIL import Image
except ImportError:
    Image = None

set_send_to_gpu(False)


def relpath(name):
    base = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(base, name)


def send_command(screen, cmd, payload=b''):
    cmd = '\033_G' + cmd
    if payload:
        if isinstance(payload, str):
            payload = payload.encode('utf-8')
        payload = standard_b64encode(payload).decode('ascii')
        cmd += ';' + payload
    cmd += '\033\\'
    c = screen.callbacks
    c.clear()
    parse_bytes(screen, cmd.encode('ascii'))
    return c.wtcbuf


def parse_response(res):
    if not res:
        return
    return res.decode('ascii').partition(';')[2].partition('\033')[0]


def parse_response_with_ids(res):
    if not res:
        return
    a, b = res.decode('ascii').split(';', 1)
    code = b.partition('\033')[0].split(':', 1)[0]
    a = a.split('G', 1)[1]
    return code, a


all_bytes = bytes(bytearray(range(256)))


def byte_block(sz):
    d, m = divmod(sz, len(all_bytes))
    return (all_bytes * d) + all_bytes[:m]


def load_helpers(self):
    s = self.create_screen()
    g = s.grman

    def pl(payload, **kw):
        kw.setdefault('i', 1)
        cmd = ','.join('%s=%s' % (k, v) for k, v in kw.items())
        res = send_command(s, cmd, payload)
        return parse_response(res)

    def sl(payload, **kw):
        if isinstance(payload, str):
            payload = payload.encode('utf-8')
        data = kw.pop('expecting_data', payload)
        cid = kw.setdefault('i', 1)
        self.ae('OK', pl(payload, **kw))
        img = g.image_for_client_id(cid)
        self.ae(img['client_id'], cid)
        self.ae(img['data'], data)
        if 's' in kw:
            self.ae((kw['s'], kw['v']), (img['width'], img['height']))
        self.ae(img['is_4byte_aligned'], kw.get('f') != 24)
        return img

    return s, g, pl, sl


def put_helpers(self, cw, ch):
    iid = 0

    def create_screen():
        s = self.create_screen(10, 5, cell_width=cw, cell_height=ch)
        return s, 2 / s.columns, 2 / s.lines

    def put_cmd(z=0, num_cols=0, num_lines=0, x_off=0, y_off=0, width=0, height=0, cell_x_off=0, cell_y_off=0, placement_id=0):
        return 'z=%d,c=%d,r=%d,x=%d,y=%d,w=%d,h=%d,X=%d,Y=%d,p=%d' % (
            z, num_cols, num_lines, x_off, y_off, width, height, cell_x_off, cell_y_off, placement_id)

    def put_image(screen, w, h, **kw):
        nonlocal iid
        iid += 1
        imgid = kw.pop('id', None) or iid
        cmd = 'a=T,f=24,i=%d,s=%d,v=%d,%s' % (imgid, w, h, put_cmd(**kw))
        data = b'x' * w * h * 3
        res = send_command(screen, cmd, data)
        return imgid, parse_response(res)

    def put_ref(screen, **kw):
        imgid = kw.pop('id', None) or iid
        cmd = 'a=p,i=%d,%s' % (imgid, put_cmd(**kw))
        return imgid, parse_response_with_ids(send_command(screen, cmd))

    def layers(screen, scrolled_by=0, xstart=-1, ystart=1):
        return screen.grman.update_layers(scrolled_by, xstart, ystart, dx, dy, screen.columns, screen.lines, cw, ch)

    def rect_eq(r, left, top, right, bottom):
        for side in 'left top right bottom'.split():
            a, b = r[side], locals()[side]
            if abs(a - b) > 0.0001:
                self.ae(a, b, 'the %s side is not equal' % side)

    s, dx, dy = create_screen()
    return s, dx, dy, put_image, put_ref, layers, rect_eq


class TestGraphics(BaseTest):

    def setUp(self):
        self.cache_dir = cache_dir.override_dir = tempfile.mkdtemp()
        self.disk_cache_dir = os.path.join(self.cache_dir, 'disk-cache')

    def tearDown(self):
        shutil.rmtree(self.cache_dir)
        cache_dir.override_dir = None

    def test_xor_data(self):

        def xor(skey, data):
            ckey = cycle(bytearray(skey))
            return bytes(bytearray(k ^ d for k, d in zip(ckey, bytearray(data))))

        base_data = os.urandom(64)
        key = os.urandom(len(base_data))
        for base in (b'', base_data):
            for extra in range(len(base_data)):
                data = base + base_data[:extra]
                self.assertEqual(xor_data(key, data), xor(key, data))

    def test_load_images(self):
        s, g, l, sl = load_helpers(self)

        # Test load query
        self.ae(l('abcd', s=1, v=1, a='q'), 'OK')
        self.assertIsNone(l('abcd', s=1, v=1, a='q', q=1))
        self.ae(g.image_count, 0)

        # Test simple load
        for f in 32, 24:
            p = 'abc' + ('d' if f == 32 else '')
            img = sl(p, s=1, v=1, f=f)
            self.ae(bool(img['is_4byte_aligned']), f == 32)

        # Test chunked load
        self.assertIsNone(l('abcd', s=2, v=2, m=1))
        self.assertIsNone(l('efgh', m=1))
        self.assertIsNone(l('ijkl', m=1))
        self.ae(l('mnop', m=0), 'OK')
        img = g.image_for_client_id(1)
        self.ae(img['data'], b'abcdefghijklmnop')
        self.ae(l('abcd', s=10, v=10, q=1), 'ENODATA:Insufficient image data: 4 < 400')
        self.ae(l('abcd', s=10, v=10, q=2), None)

        # Test compression
        random_data = byte_block(3 * 1024)
        compressed_random_data = zlib.compress(random_data)
        sl(
            compressed_random_data,
            s=24,
            v=32,
            o='z',
            expecting_data=random_data
        )

        # Test chunked + compressed
        b = len(compressed_random_data) // 2
        self.assertIsNone(l(compressed_random_data[:b], s=24, v=32, o='z', m=1))
        self.ae(l(compressed_random_data[b:], m=0), 'OK')
        img = g.image_for_client_id(1)
        self.ae(img['data'], random_data)

        # Test loading from file
        f = tempfile.NamedTemporaryFile()
        f.write(random_data), f.flush()
        sl(f.name, s=24, v=32, t='f', expecting_data=random_data)
        self.assertTrue(os.path.exists(f.name))
        f.seek(0), f.truncate(), f.write(compressed_random_data), f.flush()
        sl(f.name, s=24, v=32, t='t', o='z', expecting_data=random_data)
        self.assertRaises(
            FileNotFoundError, f.close
        )  # check that file was deleted

        # Test loading from POSIX SHM
        name = '/kitty-test-shm'
        shm_write(name, random_data)
        sl(name, s=24, v=32, t='s', expecting_data=random_data)
        self.assertRaises(
            FileNotFoundError, shm_unlink, name
        )  # check that file was deleted

    @unittest.skipIf(Image is None, 'PIL not available, skipping PNG tests')
    def test_load_png(self):
        s, g, l, sl = load_helpers(self)
        w, h = 5, 3
        rgba_data = byte_block(w * h * 4)
        img = Image.frombytes('RGBA', (w, h), rgba_data)
        rgb_data = img.convert('RGB').convert('RGBA').tobytes()

        def png(mode='RGBA'):
            buf = BytesIO()
            i = img
            if mode != i.mode:
                i = img.convert(mode)
            i.save(buf, 'PNG')
            return buf.getvalue()

        for mode in 'RGBA RGB'.split():
            data = png(mode)
            sl(data, f=100, expecting_data=rgb_data if mode == 'RGB' else rgba_data)

        for m in 'LP':
            img = img.convert(m)
            rgba_data = img.convert('RGBA').tobytes()
            data = png(m)
        sl(data, f=100, expecting_data=rgba_data)

        self.ae(l(b'a' * 20, f=100, S=20).partition(':')[0], 'EBADPNG')

    def test_load_png_simple(self):
        # 1x1 transparent PNG
        png_data = standard_b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+P+/HgAFhAJ/wlseKgAAAABJRU5ErkJggg==')
        expected = b'\x00\xff\xff\x7f'
        self.ae(load_png_data(png_data), (expected, 1, 1))
        s, g, l, sl = load_helpers(self)
        sl(png_data, f=100, expecting_data=expected)
        # test error handling for loading bad png data
        self.assertRaisesRegex(ValueError, '[EBADPNG]', load_png_data, b'dsfsdfsfsfd')

    def test_gr_operations_with_numbers(self):
        s = self.create_screen()
        g = s.grman

        def li(payload, **kw):
            cmd = ','.join('%s=%s' % (k, v) for k, v in kw.items())
            res = send_command(s, cmd, payload)
            return parse_response_with_ids(res)

        code, ids = li('abc', s=1, v=1, f=24, I=1, i=3)
        self.ae(code, 'EINVAL')

        code, ids = li('abc', s=1, v=1, f=24, I=1)
        self.ae((code, ids), ('OK', 'i=1,I=1'))
        img = g.image_for_client_number(1)
        self.ae(img['client_number'], 1)
        self.ae(img['client_id'], 1)
        code, ids = li('abc', s=1, v=1, f=24, I=1)
        self.ae((code, ids), ('OK', 'i=2,I=1'))
        img = g.image_for_client_number(1)
        self.ae(img['client_number'], 1)
        self.ae(img['client_id'], 2)
        code, ids = li('abc', s=1, v=1, f=24, I=1)
        self.ae((code, ids), ('OK', 'i=3,I=1'))
        code, ids = li('abc', s=1, v=1, f=24, i=5)
        self.ae((code, ids), ('OK', 'i=5'))
        code, ids = li('abc', s=1, v=1, f=24, I=3)
        self.ae((code, ids), ('OK', 'i=4,I=3'))

        # Test chunked load with number
        self.assertIsNone(li('abcd', s=2, v=2, m=1, I=93))
        self.assertIsNone(li('efgh', m=1))
        self.assertIsNone(li('ijkx', m=1))
        self.ae(li('mnop', m=0), ('OK', 'i=6,I=93'))
        img = g.image_for_client_number(93)
        self.ae(img['data'], b'abcdefghijkxmnop')
        self.ae(img['client_id'], 6)

        # test put with number
        def put(**kw):
            cmd = ','.join('%s=%s' % (k, v) for k, v in kw.items())
            cmd = 'a=p,' + cmd
            return parse_response_with_ids(send_command(s, cmd))

        code, idstr = put(c=2, r=2, I=93)
        self.ae((code, idstr), ('OK', 'i=6,I=93'))
        code, idstr = put(c=2, r=2, I=94)
        self.ae(code, 'ENOENT')

        # test delete with number
        def delete(ac='N', **kw):
            cmd = 'a=d'
            if ac:
                cmd += ',d={}'.format(ac)
            if kw:
                cmd += ',' + ','.join('{}={}'.format(k, v) for k, v in kw.items())
            send_command(s, cmd)

        count = s.grman.image_count
        put(i=1), put(i=2), put(i=3), put(i=4), put(i=5)
        delete(I=94)
        self.ae(s.grman.image_count, count)
        delete(I=93)
        self.ae(s.grman.image_count, count - 1)
        delete(I=1)
        self.ae(s.grman.image_count, count - 2)

    def test_image_put(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        self.ae(put_image(s, 10, 20)[1], 'OK')
        l0 = layers(s)
        self.ae(len(l0), 1)
        rect_eq(l0[0]['src_rect'], 0, 0, 1, 1)
        rect_eq(l0[0]['dest_rect'], -1, 1, -1 + dx, 1 - dy)
        self.ae(l0[0]['group_count'], 1)
        self.ae(s.cursor.x, 1), self.ae(s.cursor.y, 0)
        iid, (code, idstr) = put_ref(s, num_cols=s.columns, x_off=2, y_off=1, width=3, height=5, cell_x_off=3, cell_y_off=1, z=-1, placement_id=17)
        self.ae(idstr, f'i={iid},p=17')
        l2 = layers(s)
        self.ae(len(l2), 2)
        rect_eq(l2[0]['src_rect'], 2 / 10, 1 / 20, (2 + 3) / 10, (1 + 5)/20)
        left, top = -1 + dx + 3 * dx / cw, 1 - 1 * dy / ch
        rect_eq(l2[0]['dest_rect'], left, top, -1 + (1 + s.columns) * dx, top - dy * 5 / ch)
        rect_eq(l2[1]['src_rect'], 0, 0, 1, 1)
        rect_eq(l2[1]['dest_rect'], -1, 1, -1 + dx, 1 - dy)
        self.ae(l2[0]['group_count'], 1), self.ae(l2[1]['group_count'], 1)
        self.ae(s.cursor.x, 0), self.ae(s.cursor.y, 1)

    def test_gr_scroll(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        put_image(s, 10, 20)  # a one cell image at (0, 0)
        self.ae(len(layers(s)), 1)
        for i in range(s.lines):
            s.index()
        self.ae(len(layers(s)), 0), self.ae(s.grman.image_count, 1)
        for i in range(s.historybuf.ynum - 1):
            s.index()
            self.ae(len(layers(s)), 0), self.ae(s.grman.image_count, 1)
        s.index()
        self.ae(s.grman.image_count, 0)

        # Now test with margins
        s.reset()
        # Test images outside page area untouched
        put_image(s, cw, ch)  # a one cell image at (0, 0)
        for i in range(s.lines - 1):
            s.index()
        put_image(s, cw, ch)  # a one cell image at (0, bottom)
        s.set_margins(2, 4)  # 1-based indexing
        self.ae(s.grman.image_count, 2)
        for i in range(s.lines + s.historybuf.ynum):
            s.index()
            self.ae(s.grman.image_count, 2)
        for i in range(s.lines):  # ensure cursor is at top margin
            s.reverse_index()
        # Test clipped scrolling during index
        put_image(s, cw, 2*ch, z=-1)  # 1x2 cell image
        self.ae(s.grman.image_count, 3)
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})
        s.index(), s.index()
        l0 = layers(s)
        self.ae(len(l0), 3)
        self.ae(layers(s)[0]['src_rect'],  {'left': 0.0, 'top': 0.5, 'right': 1.0, 'bottom': 1.0})
        s.index()
        self.ae(s.grman.image_count, 2)
        # Test clipped scrolling during reverse_index
        for i in range(s.lines):
            s.reverse_index()
        put_image(s, cw, 2*ch, z=-1)  # 1x2 cell image
        self.ae(s.grman.image_count, 3)
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})
        while s.cursor.y != 1:
            s.reverse_index()
        s.reverse_index()
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.5})
        s.reverse_index()
        self.ae(s.grman.image_count, 2)

    def test_gr_reset(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        put_image(s, cw, ch)  # a one cell image at (0, 0)
        self.ae(len(layers(s)), 1)
        s.reset()
        self.ae(s.grman.image_count, 0)
        put_image(s, cw, ch)  # a one cell image at (0, 0)
        self.ae(s.grman.image_count, 1)
        for i in range(s.lines):
            s.index()
        s.reset()
        self.ae(s.grman.image_count, 1)

    def test_gr_delete(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)

        def delete(ac=None, **kw):
            cmd = 'a=d'
            if ac:
                cmd += ',d={}'.format(ac)
            if kw:
                cmd += ',' + ','.join('{}={}'.format(k, v) for k, v in kw.items())
            send_command(s, cmd)

        put_image(s, cw, ch)
        delete()
        self.ae(len(layers(s)), 0), self.ae(s.grman.image_count, 1)
        delete('A')
        self.ae(s.grman.image_count, 0)
        iid = put_image(s, cw, ch)[0]
        delete('I', i=iid, p=7)
        self.ae(s.grman.image_count, 1)
        delete('I', i=iid)
        self.ae(s.grman.image_count, 0)
        iid = put_image(s, cw, ch, placement_id=9)[0]
        delete('I', i=iid, p=9)
        self.ae(s.grman.image_count, 0)
        s.reset()
        put_image(s, cw, ch)
        put_image(s, cw, ch)
        delete('C')
        self.ae(s.grman.image_count, 2)
        s.cursor_position(1, 1)
        delete('C')
        self.ae(s.grman.image_count, 1)
        delete('P', x=2, y=1)
        self.ae(s.grman.image_count, 0)
        put_image(s, cw, ch, z=9)
        delete('Z', z=9)
        self.ae(s.grman.image_count, 0)

        # test put + delete + put
        iid = 999999
        self.ae(put_image(s, cw, ch, id=iid), (iid, 'OK'))
        self.ae(put_ref(s, id=iid), (iid, ('OK', f'i={iid}')))
        delete('i', i=iid)
        self.ae(s.grman.image_count, 1)
        self.ae(put_ref(s, id=iid), (iid, ('OK', f'i={iid}')))
        delete('I', i=iid)
        self.ae(put_ref(s, id=iid), (iid, ('ENOENT', f'i={iid}')))
        self.ae(s.grman.image_count, 0)
