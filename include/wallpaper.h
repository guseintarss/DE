#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>

struct mywm_wallpaper {
    struct wlr_buffer *buffer;
    int width;
    int height;
    void *pixels;
    /* Кэш текстуры для blur-рендера (создаётся лениво). */
    struct wlr_texture *texture;
};

struct mywm_server;
struct mywm_output;

struct mywm_wallpaper *wallpaper_load(void);
struct mywm_wallpaper *wallpaper_load_file(const char *path);
struct mywm_wallpaper *wallpaper_load_auto(struct mywm_server *server);
struct wlr_texture *wallpaper_ensure_texture(struct mywm_server *server);
void wallpaper_apply(struct mywm_server *server, struct mywm_output *output);
void wallpaper_destroy(struct mywm_wallpaper *wallpaper);

#endif