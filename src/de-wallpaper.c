/*
 * de-wallpaper — обои для DE поверх wayfire.
 *
 * Небольшой Wayland-клиент: рисует картинку в слое background через
 * zwlr-layer-shell-v1. Заменяет wallpaper.c старого композитора — теперь
 * wallpaper это не часть композитора, а отдельный процесс, которым можно
 * управлять независимо (перезапускать, менять картинку).
 *
 * Использование:
 *   de-wallpaper                    # встроенные обои (assets/wallpaper.png)
 *   DE_WALLPAPER=/путь/файл.png de-wallpaper
 *   DE_WALLPAPER_MODE=cover|stretch|fit|tile
 *
 * Режимы:
 *   cover    — заполнить экран с обрезкой (по умолчанию),
 *   stretch  — растянуть без сохранения пропорций,
 *   fit      — вписать целиком, поля остаются прозрачными,
 *   tile     — повторить в исходном размере сеткой.
 */

#define _GNU_SOURCE
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cairo.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Встроенные обои (assets/wallpaper.png), зашиты через xxd. */
extern const unsigned char wallpaper_png[];
extern const unsigned int wallpaper_png_len;

struct output;

struct buffer_set {
    struct output *output;
    struct wl_buffer *wl_buffer;
    bool released;
    bool is_current;
    struct buffer_set *next;
};

struct output {
    struct wl_output *wl_output;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_surface *surface;
    struct buffer_set *buffers;      /* все живые буферы слоя */
    struct buffer_set *current;
    int scale;
    int width, height;      /* конфигурированные размеры слоя */
    bool configured;
    struct output *next;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *wl_shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct output *outputs;

static cairo_surface_t *wallpaper_source;
static int wp_width, wp_height;

enum wallpaper_mode {
    WP_COVER,
    WP_STRETCH,
    WP_FIT,
    WP_TILE,
};

static enum wallpaper_mode wallpaper_mode = WP_COVER;

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    struct buffer_set *bs = data;
    bs->released = true;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static void pruned_buffers(struct output *out)
{
    struct buffer_set **p = &out->buffers;
    while (*p != NULL) {
        struct buffer_set *bs = *p;
        if (bs->released && !bs->is_current) {
            *p = bs->next;
            wl_buffer_destroy(bs->wl_buffer);
            free(bs);
        } else {
            p = &bs->next;
        }
    }
}

static void render_to_buffer(struct buffer_set *bs, int width, int height)
{
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                      width, height);
    cairo_t *cr = cairo_create(dst);

    double dw = width;
    double dh = height;
    double sw = wp_width;
    double sh = wp_height;

    if (wallpaper_mode == WP_TILE) {
        cairo_pattern_t *pat = cairo_pattern_create_for_surface(wallpaper_source);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);
    } else if (wallpaper_mode == WP_STRETCH) {
        cairo_scale(cr, dw / sw, dh / sh);
        cairo_set_source_surface(cr, wallpaper_source, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
    } else {
        double k = (wallpaper_mode == WP_COVER) ? fmax(dw / sw, dh / sh)
                                                : fmin(dw / sw, dh / sh);
        double ox = (dw - sw * k) / 2.0;
        double oy = (dh - sh * k) / 2.0;
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, wallpaper_source, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
    }

    cairo_destroy(cr);

    unsigned char *pixels = cairo_image_surface_get_data(dst);
    int stride = cairo_image_surface_get_stride(dst);

    int shm_fd = shm_open("/de-wallpaper", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (shm_fd < 0) {
        fprintf(stderr, "de-wallpaper: shm_open: %s\n", strerror(errno));
        cairo_surface_destroy(dst);
        return;
    }
    shm_unlink("/de-wallpaper");

    size_t size = (size_t)stride * height;
    if (ftruncate(shm_fd, (off_t)size) < 0) {
        fprintf(stderr, "de-wallpaper: ftruncate: %s\n", strerror(errno));
        close(shm_fd);
        cairo_surface_destroy(dst);
        return;
    }

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "de-wallpaper: mmap: %s\n", strerror(errno));
        close(shm_fd);
        cairo_surface_destroy(dst);
        return;
    }
    /* Скопировать отрендеренные пиксели в буфер wl_shm. */
    for (int y = 0; y < height; y++) {
        memcpy((char *)data + (size_t)y * stride,
               pixels + (size_t)y * stride, (size_t)stride);
    }
    munmap(data, size);

    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, shm_fd, size);
    bs->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
                                              stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(shm_fd);

    wl_buffer_add_listener(bs->wl_buffer, &buffer_listener, bs);
    cairo_surface_destroy(dst);
    bs->released = false;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t width, uint32_t height)
{
    struct output *out = data;

    if (width == 0 || height == 0 || wallpaper_source == NULL) {
        /* Слой ещё не получил размер — подтвердить и отпустить пустым буфером. */
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        wl_surface_attach(out->surface, NULL, 0, 0);
        wl_surface_commit(out->surface);
        return;
    }

    int pw = (int)width * out->scale;
    int ph = (int)height * out->scale;

    pruned_buffers(out);

    struct buffer_set *nb = calloc(1, sizeof(*nb));
    nb->output = out;
    render_to_buffer(nb, pw, ph);

    if (out->current != NULL) {
        out->current->is_current = false;
    }
    out->current = nb;
    nb->is_current = true;
    nb->next = out->buffers;
    out->buffers = nb;

    wl_surface_set_buffer_scale(out->surface, out->scale);
    zwlr_layer_surface_v1_set_size(layer, width, height);
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    wl_surface_attach(out->surface, nb->wl_buffer, 0, 0);
    wl_surface_damage_buffer(out->surface, 0, 0, pw, ph);
    wl_surface_commit(out->surface);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)data;
    zwlr_layer_surface_v1_destroy(layer);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void output_geometry(void *data, struct wl_output *wl_output, int x,
                            int y, int physical_width, int physical_height,
                            int subpixel, const char *make, const char *model,
                            int transform)
{
    (void)data; (void)wl_output; (void)x; (void)y;
    (void)physical_width; (void)physical_height; (void)subpixel;
    (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                        int width, int height, int refresh)
{
    (void)data; (void)wl_output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *wl_output)
{
    (void)data; (void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct output *out = data;
    out->scale = factor;
    (void)wl_output;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name)
{
    (void)data; (void)wl_output; (void)name;
}

static void output_description(void *data, struct wl_output *wl_output,
                               const char *description)
{
    (void)data; (void)wl_output; (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    (void)data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(registry, name,
                                       &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct output *out = calloc(1, sizeof(*out));
        out->wl_output = wl_registry_bind(registry, name, &wl_output_interface, 4);
        out->scale = 1;
        wl_output_add_listener(out->wl_output, &output_listener, out);
        out->next = outputs;
        outputs = out;
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* Конвертировать RGBA (stb_image) в premultiplied ARGB (cairo/BGRA в памяти). */
static unsigned char *convert_rgba(const unsigned char *src, int w, int h)
{
    unsigned char *dst = malloc((size_t)w * h * 4);
    if (dst == NULL) {
        return NULL;
    }
    for (int i = 0; i < w * h; i++) {
        unsigned char r = src[i * 4 + 0];
        unsigned char g = src[i * 4 + 1];
        unsigned char b = src[i * 4 + 2];
        unsigned char a = src[i * 4 + 3];
        dst[i * 4 + 0] = (unsigned char)((b * a) / 255);
        dst[i * 4 + 1] = (unsigned char)((g * a) / 255);
        dst[i * 4 + 2] = (unsigned char)((r * a) / 255);
        dst[i * 4 + 3] = a;
    }
    return dst;
}

static bool load_wallpaper(const char *path)
{
    int w, h, channels;
    unsigned char *pixels = NULL;
    unsigned char *converted = NULL;

    if (path != NULL && path[0] != '\0') {
        pixels = stbi_load(path, &w, &h, &channels, 4);
        if (pixels != NULL) {
            fprintf(stderr, "de-wallpaper: loaded '%s' %dx%d\n", path, w, h);
        }
    }
    if (pixels == NULL) {
        pixels = stbi_load_from_memory(wallpaper_png, wallpaper_png_len,
                                       &w, &h, &channels, 4);
        if (pixels != NULL) {
            fprintf(stderr, "de-wallpaper: loaded embedded %dx%d\n", w, h);
        }
    }
    if (pixels == NULL) {
        fprintf(stderr, "de-wallpaper: failed to decode wallpaper: %s\n",
                stbi_failure_reason());
        return false;
    }

    converted = convert_rgba(pixels, w, h);
    stbi_image_free(pixels);
    if (converted == NULL) {
        return false;
    }

    wallpaper_source = cairo_image_surface_create_for_data(
        converted, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(wallpaper_source) != CAIRO_STATUS_SUCCESS) {
        free(converted);
        return false;
    }
    wp_width = w;
    wp_height = h;
    return true;
}

static const char *parse_mode(const char *s)
{
    if (s == NULL) {
        return "cover";
    }
    if (strcmp(s, "stretch") == 0) {
        wallpaper_mode = WP_STRETCH;
    } else if (strcmp(s, "fit") == 0) {
        wallpaper_mode = WP_FIT;
    } else if (strcmp(s, "tile") == 0) {
        wallpaper_mode = WP_TILE;
    } else {
        wallpaper_mode = WP_COVER;
    }
    return s;
}

static bool create_layer_surface(struct output *out)
{
    out->surface = wl_compositor_create_surface(compositor);
    out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, out->surface, out->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "de-wallpaper");

    zwlr_layer_surface_v1_set_anchor(out->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        out->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(out->layer_surface, &layer_listener, out);

    /* Первый коммит без буфера — запрос конфигурации. */
    wl_surface_commit(out->surface);
    return true;
}

int main(int argc, char **argv)
{
    const char *path = getenv("DE_WALLPAPER");
    parse_mode(getenv("DE_WALLPAPER_MODE"));
    (void)argc; (void)argv;

    if (!load_wallpaper(path)) {
        return 1;
    }

    display = wl_display_connect(NULL);
    if (display == NULL) {
        fprintf(stderr, "de-wallpaper: cannot connect to compositor %s\n",
                getenv("WAYLAND_DISPLAY"));
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (wl_shm == NULL || layer_shell == NULL) {
        fprintf(stderr, "de-wallpaper: compositor does not provide "
                        "wl_shm/zwlr_layer_shell_v1\n");
        return 1;
    }

    for (struct output *out = outputs; out != NULL; out = out->next) {
        create_layer_surface(out);
    }

    while (wl_display_dispatch(display) != -1) {
        /* Пусто: живём, пока композитор нас держит. */
    }

    return 0;
}