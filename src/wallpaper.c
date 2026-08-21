#include "server.h"
#include "wallpaper.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/log.h>

extern const unsigned char wallpaper_png[];
extern const unsigned int wallpaper_png_len;

#define MAX_TILES 16

struct wallpaper_buffer {
    struct wlr_buffer base;
    struct mywm_wallpaper *wallpaper;
};

static void wallpaper_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct wallpaper_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    free(buf);
}

static bool wallpaper_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    struct wallpaper_buffer *buf = wl_container_of(wlr_buffer, buf, base);
    *data = buf->wallpaper->pixels;
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)wlr_buffer->width * 4;
    return true;
}

static void wallpaper_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
    (void)wlr_buffer;
}

static const struct wlr_buffer_impl wallpaper_buffer_impl = {
    .destroy = wallpaper_buffer_destroy,
    .begin_data_ptr_access = wallpaper_buffer_begin_data_ptr_access,
    .end_data_ptr_access = wallpaper_buffer_end_data_ptr_access,
};

static void premultiply_alpha(unsigned char *pixels, int count) {
    for (int i = 0; i < count; i++) {
        unsigned char *p = pixels + i * 4;
        unsigned int a = p[3];
        p[0] = (unsigned char)((p[0] * a) / 255);
        p[1] = (unsigned char)((p[1] * a) / 255);
        p[2] = (unsigned char)((p[2] * a) / 255);
    }
}

static struct mywm_wallpaper *wallpaper_from_pixels(unsigned char *pixels,
                                                    int w, int h) {
    struct mywm_wallpaper *wallpaper = calloc(1, sizeof(*wallpaper));
    struct wallpaper_buffer *buf = calloc(1, sizeof(*buf));
    if (wallpaper == NULL || buf == NULL) {
        stbi_image_free(pixels);
        free(buf);
        free(wallpaper);
        return NULL;
    }
    premultiply_alpha(pixels, w * h);
    wallpaper->width = w;
    wallpaper->height = h;
    wallpaper->pixels = pixels;
    buf->wallpaper = wallpaper;
    wlr_buffer_init(&buf->base, &wallpaper_buffer_impl, w, h);
    wallpaper->buffer = &buf->base;
    return wallpaper;
}

/* Встроенные обои (assets/wallpaper.png, зашит в бинарник). */
struct mywm_wallpaper *wallpaper_load(void) {
    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(
            wallpaper_png, (int)wallpaper_png_len, &w, &h, &channels, 4);
    if (pixels == NULL) {
        wlr_log(WLR_ERROR, "Failed to decode embedded wallpaper: %s",
                stbi_failure_reason());
        return NULL;
    }
    struct mywm_wallpaper *wallpaper = wallpaper_from_pixels(pixels, w, h);
    if (wallpaper != NULL) {
        wlr_log(WLR_INFO, "Wallpaper loaded (embedded): %dx%d", w, h);
    }
    return wallpaper;
}

/* Обои из файла (config.toml: [wallpaper] path = "...").
 * stb_image декодирует PNG/JPEG/BMP/GIF/TGA без внешних зависимостей. */
struct mywm_wallpaper *wallpaper_load_file(const char *path) {
    int w, h, channels;
    unsigned char *pixels = stbi_load(path, &w, &h, &channels, 4);
    if (pixels == NULL) {
        wlr_log(WLR_ERROR, "Failed to load wallpaper '%s': %s",
                path, stbi_failure_reason());
        return NULL;
    }
    struct mywm_wallpaper *wallpaper = wallpaper_from_pixels(pixels, w, h);
    if (wallpaper != NULL) {
        wlr_log(WLR_INFO, "Wallpaper loaded (file): '%s' %dx%d",
                path, w, h);
    }
    return wallpaper;
}

/*
 * Выбор обоев: путь из конфига (если указан и читается) или встроенные.
 * Возвращает NULL при полном провале — тогда используется fallback-цвет.
 */
struct mywm_wallpaper *wallpaper_load_auto(struct mywm_server *server) {
    if (server->wallpaper_cfg.path != NULL &&
            server->wallpaper_cfg.path[0] != '\0') {
        struct mywm_wallpaper *wp = wallpaper_load_file(server->wallpaper_cfg.path);
        if (wp != NULL) {
            return wp;
        }
        wlr_log(WLR_INFO, "Wallpaper file failed, falling back to embedded");
    }
    return wallpaper_load();
}

/* Текстура обоев для blur-рендера (кэшируется, создаётся лениво). */
struct wlr_texture *wallpaper_ensure_texture(struct mywm_server *server) {
    struct mywm_wallpaper *wp = server->wallpaper;
    if (wp == NULL) {
        return NULL;
    }
    if (wp->texture == NULL && wp->buffer != NULL) {
        wp->texture = wlr_texture_from_buffer(server->renderer, wp->buffer);
        if (wp->texture != NULL) {
            wlr_log(WLR_DEBUG, "wallpaper texture created: %dx%d",
                    wp->width, wp->height);
        }
    }
    return wp->texture;
}

/*
 * Режимы отображения обоев. Вызывается из output_frame_handler:
 *   cover   — заполнить экран с обрезкой (по умолчанию),
 *   stretch — растянуть без сохранения пропорций,
 *   fit     — вписать целиком, поля заливаются fallback-цветом,
 *   tile    — повторить в исходном размере сеткой.
 */
void wallpaper_apply(struct mywm_server *server, struct mywm_output *output) {
    struct wlr_output *wlr_output = output->wlr_output;
    if (server->wallpaper == NULL || output->background == NULL) {
        if (output->background_fallback != NULL) {
            wlr_scene_rect_set_size(output->background_fallback,
                                    wlr_output->width, wlr_output->height);
        }
        return;
    }
    int ow = wlr_output->width;
    int oh = wlr_output->height;
    double ww = server->wallpaper->width;
    double wh = server->wallpaper->height;
    enum wallpaper_mode mode = server->wallpaper_cfg.mode;

    /* Режим tile: сетка из тайлов исходного размера. */
    if (mode == WALLPAPER_MODE_TILE) {
        if (output->background != NULL) {
            wlr_scene_node_set_enabled(&output->background->node, false);
        }
        int cols = ow / (int)ww + 1;
        int rows = oh / (int)wh + 1;
        if (cols > 4 || rows > 4) {
            cols = 1;
            rows = 1;
        }
        int count = cols * rows;
        if (count > MAX_TILES) {
            count = MAX_TILES;
        }
        for (int i = 0; i < count; i++) {
            if (output->tiles[i] == NULL) {
                output->tiles[i] = wlr_scene_buffer_create(output->bg_tree,
                                                           NULL);
                if (output->tiles[i] == NULL) {
                    break;
                }
            }
            wlr_scene_buffer_set_buffer(output->tiles[i],
                                        server->wallpaper->buffer);
            wlr_scene_buffer_set_dest_size(output->tiles[i], (int)ww, (int)wh);
            wlr_scene_node_set_position(&output->tiles[i]->node,
                                        (int)((i % cols) * ww),
                                        (int)((i / cols) * wh));
            wlr_scene_node_set_enabled(&output->tiles[i]->node, true);
        }
        output->tile_count = count;
        return;
    }
    /* Остальные режимы: один буфер. */
    for (int i = 0; i < output->tile_count; i++) {
        if (output->tiles[i] != NULL) {
            wlr_scene_node_set_enabled(&output->tiles[i]->node, false);
        }
    }
    output->tile_count = 0;
    wlr_scene_node_set_enabled(&output->background->node, true);

    struct wlr_fbox src = {0, 0, ww, wh};
    struct wlr_box dst = {0, 0, ow, oh};
    switch (mode) {
    case WALLPAPER_MODE_COVER:
        if (ww / wh > (double)ow / oh) {
            src.width = wh * ow / oh;
            src.height = wh;
        } else {
            src.width = ww;
            src.height = ww * oh / ow;
        }
        src.x = (ww - src.width) / 2.0;
        src.y = (wh - src.height) / 2.0;
        break;
    case WALLPAPER_MODE_FIT: {
        double scale = (ww / wh > (double)ow / oh) ?
            ow / ww : oh / wh;
        dst.width = (int)(ww * scale);
        dst.height = (int)(wh * scale);
        dst.x = (ow - dst.width) / 2;
        dst.y = (oh - dst.height) / 2;
        break;
    }
    case WALLPAPER_MODE_STRETCH:
        break;
    case WALLPAPER_MODE_TILE:
        break;
    }
    wlr_scene_buffer_set_source_box(output->background, &src);
    wlr_scene_buffer_set_dest_size(output->background, dst.width, dst.height);
    wlr_scene_node_set_position(&output->background->node, dst.x, dst.y);
}

void wallpaper_destroy(struct mywm_wallpaper *wallpaper) {
    if (wallpaper == NULL) {
        return;
    }
    if (wallpaper->texture != NULL) {
        wlr_texture_destroy(wallpaper->texture);
    }
    wlr_buffer_drop(wallpaper->buffer);
    stbi_image_free(wallpaper->pixels);
    free(wallpaper);
}