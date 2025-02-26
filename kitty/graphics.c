/*
 * graphics.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "graphics.h"
#include "state.h"
#include "disk-cache.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdlib.h>

#include <zlib.h>
#include <structmember.h>
#include "png-reader.h"
PyTypeObject GraphicsManager_Type;

#define STORAGE_LIMIT (320u * (1024u * 1024u))

#define REPORT_ERROR(...) { log_error(__VA_ARGS__); }


static bool send_to_gpu = true;

GraphicsManager*
grman_alloc() {
    GraphicsManager *self = (GraphicsManager *)GraphicsManager_Type.tp_alloc(&GraphicsManager_Type, 0);
    self->images_capacity = self->capacity = 64;
    self->images = calloc(self->images_capacity, sizeof(Image));
    self->render_data = calloc(self->capacity, sizeof(ImageRenderData));
    if (self->images == NULL || self->render_data == NULL) {
        PyErr_NoMemory();
        Py_CLEAR(self); return NULL;
    }
    self->disk_cache = create_disk_cache();
    if (!self->disk_cache) { Py_CLEAR(self); return NULL; }
    return self;
}

static inline void
free_refs_data(Image *img) {
    free(img->refs); img->refs = NULL;
    img->refcnt = 0; img->refcap = 0;
}

static inline void
free_load_data(LoadData *ld) {
    free(ld->buf); ld->buf_used = 0; ld->buf_capacity = 0;
    ld->buf = NULL;

    if (ld->mapped_file) munmap(ld->mapped_file, ld->mapped_file_sz);
    ld->mapped_file = NULL; ld->mapped_file_sz = 0;
}

static inline void
free_image(GraphicsManager *self, Image *img) {
    if (img->texture_id) free_texture(&img->texture_id);
    free_refs_data(img);
    free_load_data(&(img->load_data));
    self->used_storage -= img->used_storage;
}


static void
dealloc(GraphicsManager* self) {
    size_t i;
    if (self->images) {
        for (i = 0; i < self->image_count; i++) free_image(self, self->images + i);
        free(self->images);
    }
    free(self->render_data);
    Py_CLEAR(self->disk_cache);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static id_type internal_id_counter = 1;

static inline Image*
img_by_internal_id(GraphicsManager *self, id_type id) {
    for (size_t i = 0; i < self->image_count; i++) {
        if (self->images[i].internal_id == id) return self->images + i;
    }
    return NULL;
}

static inline Image*
img_by_client_id(GraphicsManager *self, uint32_t id) {
    for (size_t i = 0; i < self->image_count; i++) {
        if (self->images[i].client_id == id) return self->images + i;
    }
    return NULL;
}

static inline Image*
img_by_client_number(GraphicsManager *self, uint32_t number) {
    // get the newest image with the specified number
    for (size_t i = self->image_count; i-- > 0; ) {
        if (self->images[i].client_number == number) return self->images + i;
    }
    return NULL;
}


static inline void
remove_image(GraphicsManager *self, size_t idx) {
    free_image(self, self->images + idx);
    remove_i_from_array(self->images, idx, self->image_count);
    self->layers_dirty = true;
}

static inline void
remove_images(GraphicsManager *self, bool(*predicate)(Image*), id_type skip_image_internal_id) {
    for (size_t i = self->image_count; i-- > 0;) {
        Image *img = self->images + i;
        if (img->internal_id != skip_image_internal_id && predicate(img)) {
            remove_image(self, i);
        }
    }
}


// Loading image data {{{

static bool
trim_predicate(Image *img) {
    return !img->data_loaded || !img->refcnt;
}


static int
oldest_last(const void* a, const void *b) {
    monotonic_t ans = ((Image*)(b))->atime - ((Image*)(a))->atime;
    return ans < 0 ? -1 : (ans == 0 ? 0 : 1);
}

static inline void
apply_storage_quota(GraphicsManager *self, size_t storage_limit, id_type currently_added_image_internal_id) {
    // First remove unreferenced images, even if they have an id
    remove_images(self, trim_predicate, currently_added_image_internal_id);
    if (self->used_storage < storage_limit) return;

    qsort(self->images, self->image_count, sizeof(self->images[0]), oldest_last);
    while (self->used_storage > storage_limit && self->image_count > 0) {
        remove_image(self, self->image_count - 1);
    }
    if (!self->image_count) self->used_storage = 0;  // sanity check
}

static char command_response[512] = {0};

static inline void
set_command_failed_response(const char *code, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const size_t sz = sizeof(command_response)/sizeof(command_response[0]);
    const int num = snprintf(command_response, sz, "%s:", code);
    vsnprintf(command_response + num, sz - num, fmt, args);
    va_end(args);
}

// Decode formats {{{
#define ABRT(code, ...) { set_command_failed_response(#code, __VA_ARGS__); goto err; }

static inline bool
mmap_img_file(GraphicsManager UNUSED *self, Image *img, int fd, size_t sz, off_t offset) {
    if (!sz) {
        struct stat s;
        if (fstat(fd, &s) != 0) ABRT(EBADF, "Failed to fstat() the fd: %d file with error: [%d] %s", fd, errno, strerror(errno));
        sz = s.st_size;
    }
    void *addr = mmap(0, sz, PROT_READ, MAP_SHARED, fd, offset);
    if (addr == MAP_FAILED) ABRT(EBADF, "Failed to map image file fd: %d at offset: %zd with size: %zu with error: [%d] %s", fd, offset, sz, errno, strerror(errno));
    img->load_data.mapped_file = addr;
    img->load_data.mapped_file_sz = sz;
    return true;
err:
    return false;
}


static inline const char*
zlib_strerror(int ret) {
#define Z(x) case x: return #x;
    static char buf[128];
    switch(ret) {
        case Z_ERRNO:
            return strerror(errno);
        default:
            snprintf(buf, sizeof(buf)/sizeof(buf[0]), "Unknown error: %d", ret);
            return buf;
        Z(Z_STREAM_ERROR);
        Z(Z_DATA_ERROR);
        Z(Z_MEM_ERROR);
        Z(Z_BUF_ERROR);
        Z(Z_VERSION_ERROR);
    }
#undef Z
}

static inline bool
inflate_zlib(GraphicsManager UNUSED *self, Image *img, uint8_t *buf, size_t bufsz) {
    bool ok = false;
    z_stream z;
    uint8_t *decompressed = malloc(img->load_data.data_sz);
    if (decompressed == NULL) fatal("Out of memory allocating decompression buffer");
    z.zalloc = Z_NULL;
    z.zfree = Z_NULL;
    z.opaque = Z_NULL;
    z.avail_in = bufsz;
    z.next_in = (Bytef*)buf;
    z.avail_out = img->load_data.data_sz;
    z.next_out = decompressed;
    int ret;
    if ((ret = inflateInit(&z)) != Z_OK) ABRT(ENOMEM, "Failed to initialize inflate with error: %s", zlib_strerror(ret));
    if ((ret = inflate(&z, Z_FINISH)) != Z_STREAM_END) ABRT(EINVAL, "Failed to inflate image data with error: %s", zlib_strerror(ret));
    if (z.avail_out) ABRT(EINVAL, "Image data size post inflation does not match expected size");
    free_load_data(&img->load_data);
    img->load_data.buf_capacity = img->load_data.data_sz;
    img->load_data.buf = decompressed;
    img->load_data.buf_used = img->load_data.data_sz;
    ok = true;
err:
    inflateEnd(&z);
    if (!ok) free(decompressed);
    return ok;
}

static void
png_error_handler(const char *code, const char *msg) {
    set_command_failed_response(code, "%s", msg);
}

static inline bool
inflate_png(GraphicsManager UNUSED *self, Image *img, uint8_t *buf, size_t bufsz) {
    png_read_data d = {.err_handler=png_error_handler};
    inflate_png_inner(&d, buf, bufsz);
    if (d.ok) {
        free_load_data(&img->load_data);
        img->load_data.buf = d.decompressed;
        img->load_data.buf_capacity = d.sz;
        img->load_data.buf_used = d.sz;
        img->load_data.data_sz = d.sz;
        img->width = d.width; img->height = d.height;
    }
    else free(d.decompressed);
    free(d.row_pointers);
    return d.ok;
}
#undef ABRT
// }}}

static bool
add_trim_predicate(Image *img) {
    return !img->data_loaded || (!img->client_id && !img->refcnt);
}

bool
png_path_to_bitmap(const char* path, uint8_t** data, unsigned int* width, unsigned int* height, size_t* sz) {
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        log_error("The PNG image: %s could not be opened with error: %s", path, strerror(errno));
        return false;
    }
    size_t capacity = 16*1024, pos = 0;
    unsigned char *buf = malloc(capacity);
    if (!buf) { log_error("Out of memory reading PNG file at: %s", path); fclose(fp); return false; }
    while (!feof(fp)) {
        if (pos - capacity < 1024) {
            capacity *= 2;
            unsigned char *new_buf = realloc(buf, capacity);
            if (!new_buf) {
                free(buf);
                log_error("Out of memory reading PNG file at: %s", path); fclose(fp); return false;
            }
            buf = new_buf;
        }
        pos += fread(buf + pos, sizeof(char), capacity - pos, fp);
        int saved_errno = errno;
        if (ferror(fp) && saved_errno != EINTR) {
            log_error("Failed while reading from file: %s with error: %s", path, strerror(saved_errno));
            fclose(fp);
            free(buf);
            return false;
        }
    }
    fclose(fp); fp = NULL;
    png_read_data d = {0};
    inflate_png_inner(&d, buf, pos);
    free(buf);
    if (!d.ok) {
        log_error("Failed to decode PNG image at: %s", path);
        return false;
    }
    *data = d.decompressed;
    *sz = d.sz;
    *height = d.height; *width = d.width;
    return true;
}


static inline Image*
find_or_create_image(GraphicsManager *self, uint32_t id, bool *existing) {
    if (id) {
        for (size_t i = 0; i < self->image_count; i++) {
            if (self->images[i].client_id == id) {
                *existing = true;
                return self->images + i;
            }
        }
    }
    *existing = false;
    ensure_space_for(self, images, Image, self->image_count + 1, images_capacity, 64, true);
    Image *ans = self->images + self->image_count++;
    zero_at_ptr(ans);
    return ans;
}

static int
cmp_client_ids(const void* a, const void* b) {
    const uint32_t *x = a, *y = b;
    return *x - *y;
}

static inline uint32_t
get_free_client_id(const GraphicsManager *self) {
    if (!self->image_count) return 1;
    uint32_t *client_ids = malloc(sizeof(uint32_t) * self->image_count);
    size_t count = 0;
    for (size_t i = 0; i < self->image_count; i++) {
        Image *q = self->images + i;
        if (q->client_id) client_ids[count++] = q->client_id;
    }
    if (!count) { free(client_ids); return 1; }
    qsort(client_ids, count, sizeof(uint32_t), cmp_client_ids);
    uint32_t prev_id = 0, ans = 1;
    for (size_t i = 0; i < count; i++) {
        if (client_ids[i] == prev_id) continue;
        prev_id = client_ids[i];
        if (client_ids[i] != ans) break;
        ans = client_ids[i] + 1;
    }
    free(client_ids);
    return ans;
}

static Image*
handle_add_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, bool *is_dirty, uint32_t iid) {
#define ABRT(code, ...) { set_command_failed_response(#code, __VA_ARGS__); self->loading_image = 0; if (img) img->data_loaded = false; return NULL; }
#define MAX_DATA_SZ (4u * 100000000u)
    bool existing, init_img = true;
    Image *img = NULL;
    unsigned char tt = g->transmission_type ? g->transmission_type : 'd';
    enum FORMATS { RGB=24, RGBA=32, PNG=100 };
    uint32_t fmt = g->format ? g->format : RGBA;
    if (tt == 'd' && self->loading_image) init_img = false;
    if (init_img) {
        self->last_init_graphics_command = *g;
        self->last_init_graphics_command.id = iid;
        self->loading_image = 0;
        if (g->data_width > 10000 || g->data_height > 10000) ABRT(EINVAL, "Image too large");
        remove_images(self, add_trim_predicate, 0);
        img = find_or_create_image(self, iid, &existing);
        if (existing) {
            free_load_data(&img->load_data);
            img->data_loaded = false;
            free_refs_data(img);
            *is_dirty = true;
            self->layers_dirty = true;
        } else {
            img->internal_id = internal_id_counter++;
            img->client_id = iid;
            img->client_number = g->image_number;
            if (!img->client_id && img->client_number) {
                img->client_id = get_free_client_id(self);
                self->last_init_graphics_command.id = img->client_id;
            }
        }
        img->atime = monotonic(); img->used_storage = 0;
        img->width = g->data_width; img->height = g->data_height;
        switch(fmt) {
            case PNG:
                if (g->data_sz > MAX_DATA_SZ) ABRT(EINVAL, "PNG data size too large");
                img->load_data.is_4byte_aligned = true;
                img->load_data.is_opaque = false;
                img->load_data.data_sz = g->data_sz ? g->data_sz : 1024 * 100;
                break;
            case RGB:
            case RGBA:
                img->load_data.data_sz = (size_t)g->data_width * g->data_height * (fmt / 8);
                if (!img->load_data.data_sz) ABRT(EINVAL, "Zero width/height not allowed");
                img->load_data.is_4byte_aligned = fmt == RGBA || (img->width % 4 == 0);
                img->load_data.is_opaque = fmt == RGB;
                break;
            default:
                ABRT(EINVAL, "Unknown image format: %u", fmt);
        }
        if (tt == 'd') {
            if (g->more) self->loading_image = img->internal_id;
            img->load_data.buf_capacity = img->load_data.data_sz + (g->compressed ? 1024 : 10);  // compression header
            img->load_data.buf = malloc(img->load_data.buf_capacity);
            img->load_data.buf_used = 0;
            if (img->load_data.buf == NULL) {
                ABRT(ENOMEM, "Out of memory");
                img->load_data.buf_capacity = 0; img->load_data.buf_used = 0;
            }
        }
    } else {
        self->last_init_graphics_command.more = g->more;
        self->last_init_graphics_command.payload_sz = g->payload_sz;
        g = &self->last_init_graphics_command;
        tt = g->transmission_type ? g->transmission_type : 'd';
        fmt = g->format ? g->format : RGBA;
        img = img_by_internal_id(self, self->loading_image);
        if (img == NULL) {
            self->loading_image = 0;
            ABRT(EILSEQ, "More payload loading refers to non-existent image");
        }
    }
    int fd;
    static char fname[2056] = {0};
    switch(tt) {
        case 'd':  // direct
            if (img->load_data.buf_capacity - img->load_data.buf_used < g->payload_sz) {
                if (img->load_data.buf_used + g->payload_sz > MAX_DATA_SZ || fmt != PNG) ABRT(EFBIG, "Too much data");
                img->load_data.buf_capacity = MIN(2 * img->load_data.buf_capacity, MAX_DATA_SZ);
                img->load_data.buf = realloc(img->load_data.buf, img->load_data.buf_capacity);
                if (img->load_data.buf == NULL) {
                    ABRT(ENOMEM, "Out of memory");
                    img->load_data.buf_capacity = 0; img->load_data.buf_used = 0;
                }
            }
            memcpy(img->load_data.buf + img->load_data.buf_used, payload, g->payload_sz);
            img->load_data.buf_used += g->payload_sz;
            if (!g->more) { img->data_loaded = true; self->loading_image = 0; }
            break;
        case 'f': // file
        case 't': // temporary file
        case 's': // POSIX shared memory
            if (g->payload_sz > 2048) ABRT(EINVAL, "Filename too long");
            snprintf(fname, sizeof(fname)/sizeof(fname[0]), "%.*s", (int)g->payload_sz, payload);
            if (tt == 's') fd = shm_open(fname, O_RDONLY, 0);
            else fd = open(fname, O_CLOEXEC | O_RDONLY);
            if (fd == -1) ABRT(EBADF, "Failed to open file for graphics transmission with error: [%d] %s", errno, strerror(errno));
            img->data_loaded = mmap_img_file(self, img, fd, g->data_sz, g->data_offset);
            safe_close(fd, __FILE__, __LINE__);
            if (tt == 't') {
                if (global_state.boss) { call_boss(safe_delete_temp_file, "s", fname); }
                else unlink(fname);
            }
            else if (tt == 's') shm_unlink(fname);
            break;
        default:
            ABRT(EINVAL, "Unknown transmission type: %c", g->transmission_type);
    }
    if (!img->data_loaded) return NULL;
    self->loading_image = 0;
    bool needs_processing = g->compressed || fmt == PNG;
    if (needs_processing) {
        uint8_t *buf; size_t bufsz;
#define IB { if (img->load_data.buf) { buf = img->load_data.buf; bufsz = img->load_data.buf_used; } else { buf = img->load_data.mapped_file; bufsz = img->load_data.mapped_file_sz; } }
        switch(g->compressed) {
            case 'z':
                IB;
                if (!inflate_zlib(self, img, buf, bufsz)) {
                    img->data_loaded = false; return NULL;
                }
                break;
            case 0:
                break;
            default:
                ABRT(EINVAL, "Unknown image compression: %c", g->compressed);
        }
        switch(fmt) {
            case PNG:
                IB;
                if (!inflate_png(self, img, buf, bufsz)) {
                    img->data_loaded = false; return NULL;
                }
                break;
            default: break;
        }
#undef IB
        img->load_data.data = img->load_data.buf;
        if (img->load_data.buf_used < img->load_data.data_sz) {
            ABRT(ENODATA, "Insufficient image data: %zu < %zu", img->load_data.buf_used, img->load_data.data_sz);
        }
        if (img->load_data.mapped_file) {
            munmap(img->load_data.mapped_file, img->load_data.mapped_file_sz);
            img->load_data.mapped_file = NULL; img->load_data.mapped_file_sz = 0;
        }
    } else {
        if (tt == 'd') {
            if (img->load_data.buf_used < img->load_data.data_sz) {
                ABRT(ENODATA, "Insufficient image data: %zu < %zu",  img->load_data.buf_used, img->load_data.data_sz);
            } else img->load_data.data = img->load_data.buf;
        } else {
            if (img->load_data.mapped_file_sz < img->load_data.data_sz) {
                ABRT(ENODATA, "Insufficient image data: %zu < %zu",  img->load_data.mapped_file_sz, img->load_data.data_sz);
            } else img->load_data.data = img->load_data.mapped_file;
        }
    }
    size_t required_sz = (size_t)(img->load_data.is_opaque ? 3 : 4) * img->width * img->height;
    if (img->load_data.data_sz != required_sz) ABRT(EINVAL, "Image dimensions: %ux%u do not match data size: %zu, expected size: %zu", img->width, img->height, img->load_data.data_sz, required_sz);
    if (LIKELY(img->data_loaded && send_to_gpu)) {
        send_image_to_gpu(&img->texture_id, img->load_data.data, img->width, img->height, img->load_data.is_opaque, img->load_data.is_4byte_aligned, false, REPEAT_CLAMP);
        free_load_data(&img->load_data);
        self->used_storage += required_sz;
        img->used_storage = required_sz;
    }
    return img;
#undef MAX_DATA_SZ
#undef ABRT
}

static inline const char*
finish_command_response(const GraphicsCommand *g, bool data_loaded, uint32_t iid, uint32_t placement_id, uint32_t image_number) {
    static char rbuf[sizeof(command_response)/sizeof(command_response[0]) + 128];
    bool is_ok_response = !command_response[0];
    if (g->quiet) {
        if (is_ok_response || g->quiet > 1) return NULL;
    }
    if (iid || image_number) {
        if (is_ok_response) {
            if (!data_loaded) return NULL;
            snprintf(command_response, 10, "OK");
        }
        size_t pos = 0;
        rbuf[pos++] = 'G';
#define print(fmt, ...) pos += snprintf(rbuf + pos, arraysz(rbuf) - 1 - pos, fmt, __VA_ARGS__)
        if (iid) print("i=%u", iid);
        if (image_number) print(",I=%u", image_number);
        if (placement_id) print(",p=%u", placement_id);
        print(";%s", command_response);
        return rbuf;
#undef print
    }
    return NULL;
}

// }}}

// Displaying images {{{

static inline void
update_src_rect(ImageRef *ref, Image *img) {
    // The src rect in OpenGL co-ords [0, 1] with origin at top-left corner of image
    ref->src_rect.left = (float)ref->src_x / (float)img->width;
    ref->src_rect.right = (float)(ref->src_x + ref->src_width) / (float)img->width;
    ref->src_rect.top = (float)ref->src_y / (float)img->height;
    ref->src_rect.bottom = (float)(ref->src_y + ref->src_height) / (float)img->height;
}

static inline void
update_dest_rect(ImageRef *ref, uint32_t num_cols, uint32_t num_rows, CellPixelSize cell) {
    uint32_t t;
    if (num_cols == 0) {
        t = ref->src_width + ref->cell_x_offset;
        num_cols = t / cell.width;
        if (t > num_cols * cell.width) num_cols += 1;
    }
    if (num_rows == 0) {
        t = ref->src_height + ref->cell_y_offset;
        num_rows = t / cell.height;
        if (t > num_rows * cell.height) num_rows += 1;
    }
    ref->effective_num_rows = num_rows;
    ref->effective_num_cols = num_cols;
}


static uint32_t
handle_put_command(GraphicsManager *self, const GraphicsCommand *g, Cursor *c, bool *is_dirty, Image *img, CellPixelSize cell) {
    if (img == NULL) {
        if (g->id) img = img_by_client_id(self, g->id);
        else if (g->image_number) img = img_by_client_number(self, g->image_number);
        if (img == NULL) { set_command_failed_response("ENOENT", "Put command refers to non-existent image with id: %u and number: %u", g->id, g->image_number); return g->id; }
    }
    if (!img->data_loaded) { set_command_failed_response("ENOENT", "Put command refers to image with id: %u that could not load its data", g->id); return img->client_id; }
    ensure_space_for(img, refs, ImageRef, img->refcnt + 1, refcap, 16, true);
    *is_dirty = true;
    self->layers_dirty = true;
    ImageRef *ref = NULL;
    if (g->placement_id && img->client_id) {
        for (size_t i=0; i < img->refcnt; i++) {
            if (img->refs[i].client_id == g->placement_id) {
                ref = img->refs + i;
                break;
            }
        }
    }
    if (ref == NULL) {
        ref = img->refs + img->refcnt++;
        zero_at_ptr(ref);
    }
    img->atime = monotonic();
    ref->src_x = g->x_offset; ref->src_y = g->y_offset; ref->src_width = g->width ? g->width : img->width; ref->src_height = g->height ? g->height : img->height;
    ref->src_width = MIN(ref->src_width, img->width - (img->width > ref->src_x ? ref->src_x : img->width));
    ref->src_height = MIN(ref->src_height, img->height - (img->height > ref->src_y ? ref->src_y : img->height));
    ref->z_index = g->z_index;
    ref->start_row = c->y; ref->start_column = c->x;
    ref->cell_x_offset = MIN(g->cell_x_offset, cell.width - 1);
    ref->cell_y_offset = MIN(g->cell_y_offset, cell.height - 1);
    ref->num_cols = g->num_cells; ref->num_rows = g->num_lines;
    if (img->client_id) ref->client_id = g->placement_id;
    update_src_rect(ref, img);
    update_dest_rect(ref, g->num_cells, g->num_lines, cell);
    // Move the cursor, the screen will take care of ensuring it is in bounds
    c->x += ref->effective_num_cols; c->y += ref->effective_num_rows - 1;
    return img->client_id;
}

static int
cmp_by_zindex_and_image(const void *a_, const void *b_) {
    const ImageRenderData *a = (const ImageRenderData*)a_, *b = (const ImageRenderData*)b_;
    int ans = a->z_index - b->z_index;
    if (ans == 0) ans = a->image_id - b->image_id;
    return ans;
}

static inline void
set_vertex_data(ImageRenderData *rd, const ImageRef *ref, const ImageRect *dest_rect) {
#define R(n, a, b) rd->vertices[n*4] = ref->src_rect.a; rd->vertices[n*4 + 1] = ref->src_rect.b; rd->vertices[n*4 + 2] = dest_rect->a; rd->vertices[n*4 + 3] = dest_rect->b;
        R(0, right, top); R(1, right, bottom); R(2, left, bottom); R(3, left, top);
#undef R
}

void
gpu_data_for_centered_image(ImageRenderData *ans, unsigned int screen_width_px, unsigned int screen_height_px, unsigned int width, unsigned int height) {
    static const ImageRef source_rect = { .src_rect = { .left=0, .top=0, .bottom=1, .right=1 }};
    const ImageRef *ref = &source_rect;
    float width_frac = 2 * MIN(1, width / (float)screen_width_px), height_frac = 2 * MIN(1, height / (float)screen_height_px);
    float hmargin = (2 - width_frac) / 2;
    float vmargin = (2 - height_frac) / 2;
    const ImageRect r = { .left = -1 + hmargin, .right = -1 + hmargin + width_frac, .top = 1 - vmargin, .bottom = 1 - vmargin - height_frac };
    set_vertex_data(ans, ref, &r);
}

bool
grman_update_layers(GraphicsManager *self, unsigned int scrolled_by, float screen_left, float screen_top, float dx, float dy, unsigned int num_cols, unsigned int num_rows, CellPixelSize cell) {
    if (self->last_scrolled_by != scrolled_by) self->layers_dirty = true;
    self->last_scrolled_by = scrolled_by;
    if (!self->layers_dirty) return false;
    self->layers_dirty = false;
    size_t i, j;
    self->num_of_below_refs = 0;
    self->num_of_negative_refs = 0;
    self->num_of_positive_refs = 0;
    Image *img; ImageRef *ref;
    ImageRect r;
    float screen_width = dx * num_cols, screen_height = dy * num_rows;
    float screen_bottom = screen_top - screen_height;
    float screen_width_px = num_cols * cell.width;
    float screen_height_px = num_rows * cell.height;
    float y0 = screen_top - dy * scrolled_by;

    // Iterate over all visible refs and create render data
    self->count = 0;
    for (i = 0; i < self->image_count; i++) { img = self->images + i; for (j = 0; j < img->refcnt; j++) { ref = img->refs + j;
        r.top = y0 - ref->start_row * dy - dy * (float)ref->cell_y_offset / (float)cell.height;
        if (ref->num_rows > 0) r.bottom = y0 - (ref->start_row + (int32_t)ref->num_rows) * dy;
        else r.bottom = r.top - screen_height * (float)ref->src_height / screen_height_px;
        if (r.top <= screen_bottom || r.bottom >= screen_top) continue;  // not visible

        r.left = screen_left + ref->start_column * dx + dx * (float)ref->cell_x_offset / (float) cell.width;
        if (ref->num_cols > 0) r.right = screen_left + (ref->start_column + (int32_t)ref->num_cols) * dx;
        else r.right = r.left + screen_width * (float)ref->src_width / screen_width_px;

        if (ref->z_index < ((int32_t)INT32_MIN/2))
            self->num_of_below_refs++;
        else if (ref->z_index < 0)
            self->num_of_negative_refs++;
        else
            self->num_of_positive_refs++;
        ensure_space_for(self, render_data, ImageRenderData, self->count + 1, capacity, 64, true);
        ImageRenderData *rd = self->render_data + self->count;
        zero_at_ptr(rd);
        set_vertex_data(rd, ref, &r);
        self->count++;
        rd->z_index = ref->z_index; rd->image_id = img->internal_id;
        rd->texture_id = img->texture_id;
    }}
    if (!self->count) return false;
    // Sort visible refs in draw order (z-index, img)
    qsort(self->render_data, self->count, sizeof(self->render_data[0]), cmp_by_zindex_and_image);
    // Calculate the group counts
    i = 0;
    while (i < self->count) {
        id_type image_id = self->render_data[i].image_id, start = i;
        if (start == self->count - 1) i = self->count;
        else {
            while (i < self->count - 1 && self->render_data[++i].image_id == image_id) {}
        }
        self->render_data[start].group_count = i - start;
    }
    return true;
}

// }}}

// Image lifetime/scrolling {{{

static inline void
filter_refs(GraphicsManager *self, const void* data, bool free_images, bool (*filter_func)(const ImageRef*, Image*, const void*, CellPixelSize), CellPixelSize cell, bool only_first_image) {
    bool matched = false;
    for (size_t i = self->image_count; i-- > 0;) {
        Image *img = self->images + i;
        for (size_t j = img->refcnt; j-- > 0;) {
            ImageRef *ref = img->refs + j;
            if (filter_func(ref, img, data, cell)) {
                remove_i_from_array(img->refs, j, img->refcnt);
                self->layers_dirty = true;
                matched = true;
            }
        }
        if (img->refcnt == 0 && (free_images || img->client_id == 0)) remove_image(self, i);
        if (only_first_image && matched) break;
    }
}


static inline void
modify_refs(GraphicsManager *self, const void* data, bool free_images, bool (*filter_func)(ImageRef*, Image*, const void*, CellPixelSize), CellPixelSize cell) {
    for (size_t i = self->image_count; i-- > 0;) {
        Image *img = self->images + i;
        for (size_t j = img->refcnt; j-- > 0;) {
            if (filter_func(img->refs + j, img, data, cell)) remove_i_from_array(img->refs, j, img->refcnt);
        }
        if (img->refcnt == 0 && (free_images || img->client_id == 0)) remove_image(self, i);
    }
}


static inline bool
scroll_filter_func(ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    ScrollData *d = (ScrollData*)data;
    ref->start_row += d->amt;
    return ref->start_row + (int32_t)ref->effective_num_rows <= d->limit;
}

static inline bool
ref_within_region(const ImageRef *ref, index_type margin_top, index_type margin_bottom) {
    return ref->start_row >= (int32_t)margin_top && ref->start_row + ref->effective_num_rows <= margin_bottom;
}

static inline bool
ref_outside_region(const ImageRef *ref, index_type margin_top, index_type margin_bottom) {
    return ref->start_row + ref->effective_num_rows <= margin_top || ref->start_row > (int32_t)margin_bottom;
}

static inline bool
scroll_filter_margins_func(ImageRef* ref, Image* img, const void* data, CellPixelSize cell) {
    ScrollData *d = (ScrollData*)data;
    if (ref_within_region(ref, d->margin_top, d->margin_bottom)) {
        ref->start_row += d->amt;
        if (ref_outside_region(ref, d->margin_top, d->margin_bottom)) return true;
        // Clip the image if scrolling has resulted in part of it being outside the page area
        uint32_t clip_amt, clipped_rows;
        if (ref->start_row < (int32_t)d->margin_top) {
            // image moved up
            clipped_rows = d->margin_top - ref->start_row;
            clip_amt = cell.height * clipped_rows;
            if (ref->src_height <= clip_amt) return true;
            ref->src_y += clip_amt; ref->src_height -= clip_amt;
            ref->effective_num_rows -= clipped_rows;
            update_src_rect(ref, img);
            ref->start_row += clipped_rows;
        } else if (ref->start_row + ref->effective_num_rows > d->margin_bottom) {
            // image moved down
            clipped_rows = ref->start_row + ref->effective_num_rows - d->margin_bottom;
            clip_amt = cell.height * clipped_rows;
            if (ref->src_height <= clip_amt) return true;
            ref->src_height -= clip_amt;
            ref->effective_num_rows -= clipped_rows;
            update_src_rect(ref, img);
        }
        return ref_outside_region(ref, d->margin_top, d->margin_bottom);
    }
    return false;
}

void
grman_scroll_images(GraphicsManager *self, const ScrollData *data, CellPixelSize cell) {
    if (self->image_count) {
        self->layers_dirty = true;
        modify_refs(self, data, true, data->has_margins ? scroll_filter_margins_func : scroll_filter_func, cell);
    }
}

static inline bool
clear_filter_func(const ImageRef *ref, Image UNUSED *img, const void UNUSED *data, CellPixelSize cell UNUSED) {
    return ref->start_row + (int32_t)ref->effective_num_rows > 0;
}

static inline bool
clear_all_filter_func(const ImageRef *ref UNUSED, Image UNUSED *img, const void UNUSED *data, CellPixelSize cell UNUSED) {
    return true;
}

void
grman_clear(GraphicsManager *self, bool all, CellPixelSize cell) {
    filter_refs(self, NULL, true, all ? clear_all_filter_func : clear_filter_func, cell, false);
}

static inline bool
id_filter_func(const ImageRef *ref, Image *img, const void *data, CellPixelSize cell UNUSED) {
    const GraphicsCommand *g = data;
    if (g->id && img->client_id == g->id) return !g->placement_id || ref->client_id == g->placement_id;
    return false;
}

static inline bool
number_filter_func(const ImageRef *ref, Image *img, const void *data, CellPixelSize cell UNUSED) {
    const GraphicsCommand *g = data;
    if (g->image_number && img->client_number == g->image_number) return !g->placement_id || ref->client_id == g->placement_id;
    return false;
}


static inline bool
x_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    const GraphicsCommand *g = data;
    return ref->start_column <= (int32_t)g->x_offset - 1 && ((int32_t)g->x_offset - 1) < ((int32_t)(ref->start_column + ref->effective_num_cols));
}

static inline bool
y_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    const GraphicsCommand *g = data;
    return ref->start_row <= (int32_t)g->y_offset - 1 && ((int32_t)(g->y_offset - 1 < ref->start_row + ref->effective_num_rows));
}

static inline bool
z_filter_func(const ImageRef *ref, Image UNUSED *img, const void *data, CellPixelSize cell UNUSED) {
    const GraphicsCommand *g = data;
    return ref->z_index == g->z_index;
}


static inline bool
point_filter_func(const ImageRef *ref, Image *img, const void *data, CellPixelSize cell) {
    return x_filter_func(ref, img, data, cell) && y_filter_func(ref, img, data, cell);
}

static inline bool
point3d_filter_func(const ImageRef *ref, Image *img, const void *data, CellPixelSize cell) {
    return z_filter_func(ref, img, data, cell) && point_filter_func(ref, img, data, cell);
}


static void
handle_delete_command(GraphicsManager *self, const GraphicsCommand *g, Cursor *c, bool *is_dirty, CellPixelSize cell) {
    GraphicsCommand d;
    bool only_first_image = false;
    switch (g->delete_action) {
#define I(u, data, func) filter_refs(self, data, g->delete_action == u, func, cell, only_first_image); *is_dirty = true; break
#define D(l, u, data, func) case l: case u: I(u, data, func)
#define G(l, u, func) D(l, u, g, func)
        case 0:
        D('a', 'A', NULL, clear_filter_func);
        G('i', 'I', id_filter_func);
        G('p', 'P', point_filter_func);
        G('q', 'Q', point3d_filter_func);
        G('x', 'X', x_filter_func);
        G('y', 'Y', y_filter_func);
        G('z', 'Z', z_filter_func);
        case 'c':
        case 'C':
            d.x_offset = c->x + 1; d.y_offset = c->y + 1;
            I('C', &d, point_filter_func);
        case 'n':
        case 'N':
            only_first_image = true;
            I('N', g, number_filter_func);
        default:
            REPORT_ERROR("Unknown graphics command delete action: %c", g->delete_action);
            break;
#undef G
#undef D
#undef I
    }
    if (!self->image_count && self->count) self->count = 0;
}

// }}}

void
grman_resize(GraphicsManager *self, index_type UNUSED old_lines, index_type UNUSED lines, index_type UNUSED old_columns, index_type UNUSED columns) {
    self->layers_dirty = true;
}

void
grman_rescale(GraphicsManager *self, CellPixelSize cell) {
    ImageRef *ref; Image *img;
    self->layers_dirty = true;
    for (size_t i = self->image_count; i-- > 0;) {
        img = self->images + i;
        for (size_t j = img->refcnt; j-- > 0;) {
            ref = img->refs + j;
            ref->cell_x_offset = MIN(ref->cell_x_offset, cell.width - 1);
            ref->cell_y_offset = MIN(ref->cell_y_offset, cell.height - 1);
            update_dest_rect(ref, ref->num_cols, ref->num_rows, cell);
        }
    }
}

const char*
grman_handle_command(GraphicsManager *self, const GraphicsCommand *g, const uint8_t *payload, Cursor *c, bool *is_dirty, CellPixelSize cell) {
    const char *ret = NULL;
    command_response[0] = 0;

    if (g->id && g->image_number) {
        set_command_failed_response("EINVAL", "Must not specify both image id and image number");
        return finish_command_response(g, false, g->id, g->placement_id, g->image_number);
    }

    switch(g->action) {
        case 0:
        case 't':
        case 'T':
        case 'q': {
            uint32_t iid = g->id, q_iid = iid;
            bool is_query = g->action == 'q';
            if (is_query) { iid = 0; if (!q_iid) { REPORT_ERROR("Query graphics command without image id"); break; } }
            Image *image = handle_add_command(self, g, payload, is_dirty, iid);
            if (is_query) ret = finish_command_response(g, image != NULL,  q_iid, 0, 0);
            else ret = finish_command_response(g, image != NULL, self->last_init_graphics_command.id, self->last_init_graphics_command.placement_id, self->last_init_graphics_command.image_number);
            if (self->last_init_graphics_command.action == 'T' && image && image->data_loaded) handle_put_command(self, &self->last_init_graphics_command, c, is_dirty, image, cell);
            id_type added_image_id = image ? image->internal_id : 0;
            if (g->action == 'q') remove_images(self, add_trim_predicate, 0);
            if (self->used_storage > STORAGE_LIMIT) apply_storage_quota(self, STORAGE_LIMIT, added_image_id);
            break;
        }
        case 'p':
            if (!g->id && !g->image_number) {
                REPORT_ERROR("Put graphics command without image id or number");
                break;
            }
            uint32_t image_id = handle_put_command(self, g, c, is_dirty, NULL, cell);
            ret = finish_command_response(g, true, image_id, g->placement_id, g->image_number);
            break;
        case 'd':
            handle_delete_command(self, g, c, is_dirty, cell);
            break;
        default:
            REPORT_ERROR("Unknown graphics command action: %c", g->action);
            break;
    }
    return ret;
}


// Boilerplate {{{
static PyObject *
new(PyTypeObject UNUSED *type, PyObject UNUSED *args, PyObject UNUSED *kwds) {
    PyObject *ans = (PyObject*)grman_alloc();
    if (ans == NULL) PyErr_NoMemory();
    return ans;
}

static inline PyObject*
image_as_dict(Image *img) {
#define U(x) #x, img->x
    return Py_BuildValue("{sI sI sI sI sK sI sI sO sO sN}",
        U(texture_id), U(client_id), U(width), U(height), U(internal_id), U(refcnt), U(client_number),
        "data_loaded", img->data_loaded ? Py_True : Py_False,
        "is_4byte_aligned", img->load_data.is_4byte_aligned ? Py_True : Py_False,
        "data", Py_BuildValue("y#", img->load_data.data, img->load_data.data_sz)
    );
#undef U

}

#define W(x) static PyObject* py##x(GraphicsManager UNUSED *self, PyObject *args)
#define PA(fmt, ...) if(!PyArg_ParseTuple(args, fmt, __VA_ARGS__)) return NULL;

W(image_for_client_id) {
    unsigned long id = PyLong_AsUnsignedLong(args);
    bool existing = false;
    Image *img = find_or_create_image(self, id, &existing);
    if (!existing) { Py_RETURN_NONE; }
    return image_as_dict(img);
}

W(image_for_client_number) {
    unsigned long num = PyLong_AsUnsignedLong(args);
    Image *img = img_by_client_number(self, num);
    if (!img) Py_RETURN_NONE;
    return image_as_dict(img);
}

W(shm_write) {
    const char *name, *data;
    Py_ssize_t sz;
    PA("ss#", &name, &data, &sz);
    int fd = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd == -1) { PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    int ret = ftruncate(fd, sz);
    if (ret != 0) { safe_close(fd, __FILE__, __LINE__); PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    void *addr = mmap(0, sz, PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) { safe_close(fd, __FILE__, __LINE__); PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    memcpy(addr, data, sz);
    if (munmap(addr, sz) != 0) { safe_close(fd, __FILE__, __LINE__); PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    safe_close(fd, __FILE__, __LINE__);
    Py_RETURN_NONE;
}

W(shm_unlink) {
    char *name;
    PA("s", &name);
    int ret = shm_unlink(name);
    if (ret == -1) { PyErr_SetFromErrnoWithFilename(PyExc_OSError, name); return NULL; }
    Py_RETURN_NONE;
}

W(set_send_to_gpu) {
    send_to_gpu = PyObject_IsTrue(args) ? true : false;
    Py_RETURN_NONE;
}

W(update_layers) {
    unsigned int scrolled_by, sx, sy; float xstart, ystart, dx, dy;
    CellPixelSize cell;
    PA("IffffIIII", &scrolled_by, &xstart, &ystart, &dx, &dy, &sx, &sy, &cell.width, &cell.height);
    grman_update_layers(self, scrolled_by, xstart, ystart, dx, dy, sx, sy, cell);
    PyObject *ans = PyTuple_New(self->count);
    for (size_t i = 0; i < self->count; i++) {
        ImageRenderData *r = self->render_data + i;
#define R(offset) Py_BuildValue("{sf sf sf sf}", "left", r->vertices[offset + 8], "top", r->vertices[offset + 1], "right", r->vertices[offset], "bottom", r->vertices[offset + 5])
        PyTuple_SET_ITEM(ans, i,
            Py_BuildValue("{sN sN sI si sK}", "src_rect", R(0), "dest_rect", R(2), "group_count", r->group_count, "z_index", r->z_index, "image_id", r->image_id)
        );
#undef R
    }
    return ans;
}

#define M(x, va) {#x, (PyCFunction)py##x, va, ""}

static PyMethodDef methods[] = {
    M(image_for_client_id, METH_O),
    M(image_for_client_number, METH_O),
    M(update_layers, METH_VARARGS),
    {NULL}  /* Sentinel */
};

static PyMemberDef members[] = {
    {"image_count", T_PYSSIZET, offsetof(GraphicsManager, image_count), 0, "image_count"},
    {NULL},
};

PyTypeObject GraphicsManager_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fast_data_types.GraphicsManager",
    .tp_basicsize = sizeof(GraphicsManager),
    .tp_dealloc = (destructor)dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "GraphicsManager",
    .tp_new = new,
    .tp_methods = methods,
    .tp_members = members,
};

static PyMethodDef module_methods[] = {
    M(shm_write, METH_VARARGS),
    M(shm_unlink, METH_VARARGS),
    M(set_send_to_gpu, METH_O),
    {NULL, NULL, 0, NULL}        /* Sentinel */
};


bool
init_graphics(PyObject *module) {
    if (PyType_Ready(&GraphicsManager_Type) < 0) return false;
    if (PyModule_AddObject(module, "GraphicsManager", (PyObject *)&GraphicsManager_Type) != 0) return false;
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    Py_INCREF(&GraphicsManager_Type);
    return true;
}
// }}}
