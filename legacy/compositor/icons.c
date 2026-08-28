#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* Стандартные пути для иконок в Linux */
static const char *icon_paths[] = {
    "/usr/share/icons",
    "/usr/local/share/icons",
    "/home/.icons",
    "/usr/share/pixmaps",
    NULL
};

/* Темы по умолчанию */
static const char *default_themes[] = {
    "Adwaita",
    "gnome",
    "hicolor",
    NULL
};

void icon_manager_init(struct mywm_icon_manager *mgr,
                       struct wlr_renderer *renderer) {
    if (!mgr) return;

    mgr->renderer = renderer;
    mgr->icon_size = 48;

    /* Определяем тему по умолчанию */
    mgr->icon_theme = strdup("Adwaita");

    /* Проверяем наличие темы, иначе используем hicolor */
    char path[512];
    int found = 0;
    for (int i = 0; default_themes[i] != NULL; i++) {
        snprintf(path, sizeof(path), "/usr/share/icons/%s", default_themes[i]);
        if (access(path, R_OK) == 0) {
            free(mgr->icon_theme);
            mgr->icon_theme = strdup(default_themes[i]);
            found = 1;
            break;
        }
    }

    if (!found) {
        mgr->icon_theme = strdup("hicolor");
    }

    wlr_log(WLR_DEBUG, "Icon manager initialized with theme: %s", mgr->icon_theme);
}

void icon_manager_finish(struct mywm_icon_manager *mgr) {
    if (!mgr) return;

    if (mgr->icon_theme) {
        free(mgr->icon_theme);
        mgr->icon_theme = NULL;
    }
}

/*
 * Поиск иконки по спецификации Icon Theme Directory Layout:
 * базовые каталоги (.icons, XDG_DATA_HOME/icons, /usr/share/icons),
 * цепочка тем (заданная тема -> hicolor), все размерные подкаталоги,
 * отсортированные по близости к запрошенному размеру (scalable — в конце),
 * внутри каталога пробуем apps/, корень и categories/.
 */

/* Каталог размеров внутри темы. size == 0 означает scalable. */
struct icon_size_dir {
    char path[768];
    int size;
};

static int wanted_icon_size = 48;

static int cmp_icon_size_dir(const void *pa, const void *pb) {
    const struct icon_size_dir *a = pa, *b = pb;
    int da = abs(a->size - wanted_icon_size);
    int db = abs(b->size - wanted_icon_size);
    /* scalable считаем самой дальней после всех точных размеров. */
    if (a->size == 0) {
        da = 1 << 20;
    }
    if (b->size == 0) {
        db = 1 << 20;
    }
    return da - db;
}

/* Пробует имя с расширениями png и svg (последний — при наличии librsvg)
 * в подкаталогах size_dir; найденный путь пишется в out. */
static bool probe_icon_file(const char *size_dir, const char *icon_name,
                            const char *ext_pref, char *out, size_t out_len) {
    static const struct { const char *sub; } subs[] = {
        { "apps" }, { "" }, { "categories" }, { "legacy" },
    };
    for (size_t s = 0; s < sizeof(subs) / sizeof(subs[0]); s++) {
        const char *ext_order[2] = { ext_pref, NULL };
        const char *alt = strcmp(ext_pref, "png") == 0 ? "svg" : "png";
        ext_order[1] = alt;
        for (int e = 0; e < 2; e++) {
            const char *ext = ext_order[e];
            if (strcmp(ext, "svg") == 0) {
#ifndef HAVE_RSVG
                continue;
#endif
            }
            if (subs[s].sub[0] != '\0') {
                snprintf(out, out_len, "%s/%s/%s.%s", size_dir,
                         subs[s].sub, icon_name, ext);
            } else {
                snprintf(out, out_len, "%s/%s.%s", size_dir,
                         icon_name, ext);
            }
            if (access(out, R_OK) == 0) {
                return true;
            }
        }
    }
    return false;
}

static char *lookup_theme_icon(const char *base, const char *theme,
                               const char *icon_name, int size,
                               const char *ext_pref) {
    static char path[1024];
    char theme_root[512];
    snprintf(theme_root, sizeof(theme_root), "%s/%s", base, theme);
    DIR *d = opendir(theme_root);
    if (d == NULL) {
        return NULL;
    }

    struct icon_size_dir dirs[64];
    int ndirs = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && ndirs < 64) {
        if (de->d_name[0] == '.') {
            continue;
        }
        if (strcasecmp(de->d_name, "scalable") == 0) {
            snprintf(dirs[ndirs].path, sizeof(dirs[ndirs].path),
                     "%s/%s", theme_root, de->d_name);
            dirs[ndirs].size = 0;
            ndirs++;
            continue;
        }
        int sz = atoi(de->d_name);
        if (sz > 0 && strchr(de->d_name, 'x') != NULL) {
            snprintf(dirs[ndirs].path, sizeof(dirs[ndirs].path),
                     "%s/%s", theme_root, de->d_name);
            dirs[ndirs].size = sz;
            ndirs++;
        }
    }
    closedir(d);

    wanted_icon_size = size;
    qsort(dirs, ndirs, sizeof(dirs[0]), cmp_icon_size_dir);

    for (int i = 0; i < ndirs; i++) {
        if (probe_icon_file(dirs[i].path, icon_name, ext_pref,
                            path, sizeof(path))) {
            return path;
        }
    }
    return NULL;
}

/* Вспомогательная функция для поиска файла иконки */
static char *find_icon_file(const char *theme, const char *icon_name,
                            int size, const char *extension) {
    static char path[1024];

    const char *home = getenv("HOME");
    const char *xdg_data_home = getenv("XDG_DATA_HOME");

    /* Цепочка тем: заданная тема, затем hicolor (по спецификации). */
    const char *themes[3];
    int nthemes = 0;
    themes[nthemes++] = theme;
    if (strcmp(theme, "hicolor") != 0) {
        themes[nthemes++] = "hicolor";
    }

    for (int t = 0; t < nthemes; t++) {
        /* Пользовательские каталоги имеют приоритет над системными. */
        if (home != NULL) {
            snprintf(path, sizeof(path), "%s/.icons/%s", home, themes[t]);
            if (access(path, R_OK) == 0 &&
                    lookup_theme_icon(path, ".", icon_name, size,
                                      extension)) {
                return path;
            }
            char base[512];
            if (xdg_data_home != NULL && xdg_data_home[0] != '\0') {
                snprintf(base, sizeof(base), "%s/icons", xdg_data_home);
            } else {
                snprintf(base, sizeof(base),
                         "%s/.local/share/icons", home);
            }
            if (access(base, R_OK) == 0 &&
                    lookup_theme_icon(base, themes[t], icon_name, size,
                                      extension)) {
                return path;
            }
        }
        for (int b = 0; icon_paths[b] != NULL; b++) {
            if (strstr(icon_paths[b], "pixmaps") != NULL) {
                continue;
            }
            char *hit = lookup_theme_icon(icon_paths[b], themes[t],
                                          icon_name, size, extension);
            if (hit != NULL) {
                return hit;
            }
        }
    }

    return NULL;
}

/* Поиск иконки в pixmaps */
static char *find_icon_in_pixmaps(const char *icon_name, const char *extension) {
    static char path[512];
    
    snprintf(path, sizeof(path), "/usr/share/pixmaps/%s.%s", icon_name, extension);
    
    if (access(path, R_OK) == 0) {
        return path;
    }
    
    return NULL;
}

/* Загрузка изображения из файла в Cairo поверхность */
#ifdef HAVE_RSVG
#include <librsvg/rsvg.h>

static cairo_surface_t *load_svg_file(const char *filename, int size) {
    RsvgHandle *handle = rsvg_handle_new_from_file(filename, NULL);
    if (handle == NULL) {
        return NULL;
    }
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    RsvgRectangle viewport = {0.0, 0.0, (double)size, (double)size};
    gboolean ok = rsvg_handle_render_document(handle, cr, &viewport, NULL);
    cairo_destroy(cr);
    g_object_unref(handle);
    if (!ok || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }
    return surface;
}
#endif

static cairo_surface_t *load_image_from_file(const char *filename, int size) {
    cairo_surface_t *surface = NULL;

    /* Определяем формат по расширению */
    const char *ext = strrchr(filename, '.');
    if (!ext) return NULL;

    if (strcasecmp(ext, ".png") == 0) {
        surface = cairo_image_surface_create_from_png(filename);
    } else if (strcasecmp(ext, ".svg") == 0) {
#ifdef HAVE_RSVG
        surface = load_svg_file(filename, size);
#else
        wlr_log(WLR_DEBUG, "SVG support requires librsvg: %s", filename);
        return NULL;
#endif
    }
    
    if (surface && cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return NULL;
    }
    
    /* Масштабирование до нужного размера если необходимо */
    if (surface) {
        int sw = cairo_image_surface_get_width(surface);
        int sh = cairo_image_surface_get_height(surface);
        
        if (sw != size || sh != size) {
            cairo_surface_t *scaled = cairo_image_surface_create(
                CAIRO_FORMAT_ARGB32, size, size);
            cairo_t *cr = cairo_create(scaled);
            
            double scale_x = (double)size / sw;
            double scale_y = (double)size / sh;
            double scale = scale_x < scale_y ? scale_x : scale_y;
            
            double dx = (size - sw * scale) / 2.0;
            double dy = (size - sh * scale) / 2.0;
            
            cairo_scale(cr, scale, scale);
            cairo_translate(cr, dx / scale, dy / scale);
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_paint(cr);
            
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
            surface = scaled;
        }
    }
    
    return surface;
}

/* Создание scene-буфера из Cairo поверхности (wlroots 0.20: текстуры
 * загружаются через wlr_buffer с data-ptr доступом, см. effects.c). */
struct mywm_icon_buf {
    struct wlr_buffer base;
    cairo_surface_t *surface;
};

static void icon_buf_destroy(struct wlr_buffer *wb) {
    struct mywm_icon_buf *buf = wl_container_of(wb, buf, base);
    cairo_surface_destroy(buf->surface);
    free(buf);
}

static bool icon_buf_begin_data_ptr_access(struct wlr_buffer *wb,
                                           uint32_t flags, void **data,
                                           uint32_t *format, size_t *stride) {
    (void)flags;
    struct mywm_icon_buf *buf = wl_container_of(wb, buf, base);
    cairo_surface_flush(buf->surface);
    *data = cairo_image_surface_get_data(buf->surface);
    if (*data == NULL) {
        return false;
    }
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)cairo_image_surface_get_stride(buf->surface);
    return true;
}

static void icon_buf_end_data_ptr_access(struct wlr_buffer *wb) {
    (void)wb;
}

static const struct wlr_buffer_impl icon_buf_impl = {
    .destroy = icon_buf_destroy,
    .begin_data_ptr_access = icon_buf_begin_data_ptr_access,
    .end_data_ptr_access = icon_buf_end_data_ptr_access,
};

static struct wlr_scene_buffer *create_texture_from_cairo_sized(
    struct mywm_icon_manager *mgr,
    struct wlr_scene_tree *parent,
    cairo_surface_t *surface,
    int dest_w,
    int dest_h) {

    if (!surface || !mgr || !parent ||
        cairo_image_surface_get_width(surface) == 0) {
        return NULL;
    }

    struct mywm_icon_buf *ib = calloc(1, sizeof(*ib));
    if (!ib) {
        return NULL;
    }
    ib->surface = cairo_surface_reference(surface);
    wlr_buffer_init(&ib->base, &icon_buf_impl,
                    cairo_image_surface_get_width(surface),
                    cairo_image_surface_get_height(surface));

    /* Устанавливаем размер отображения */
    struct wlr_scene_buffer *scene_buf = wlr_scene_buffer_create(parent, NULL);
    if (!scene_buf) {
        wlr_buffer_drop(&ib->base);
        return NULL;
    }
    wlr_scene_buffer_set_buffer(scene_buf, &ib->base);
    wlr_scene_buffer_set_dest_size(scene_buf, dest_w, dest_h);

    /* Ссылка сцены держит буфер живым; наша ссылка больше не нужна */
    wlr_buffer_drop(&ib->base);

    /* Освобождаем Cairo поверхность */
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);

    return scene_buf;
}

static struct wlr_scene_buffer *create_texture_from_cairo(
    struct mywm_icon_manager *mgr,
    struct wlr_scene_tree *parent,
    cairo_surface_t *surface,
    int size) {
    return create_texture_from_cairo_sized(mgr, parent, surface, size, size);
}

/* Создание fallback иконки с закруглёнными углами */
struct wlr_scene_buffer *icon_create_fallback(struct mywm_icon_manager *mgr,
                                               struct wlr_scene_tree *parent,
                                               int size,
                                               const float color[4]) {
    if (!mgr || !parent) {
        return NULL;
    }
    
    /* Создаём Cairo поверхность */
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    
    /* Очищаем фон (прозрачный) */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    
    /* Рисуем закруглённый прямоугольник */
    double radius = size * 0.2;
    double x = 0, y = 0, w = size, h = size;
    
    cairo_new_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_arc(cr, x + w - radius, y + radius, radius, 3 * M_PI / 2, 2 * M_PI);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, x + radius, y + h - radius, radius, M_PI / 2, M_PI);
    cairo_close_path(cr);
    
    /* Закрашиваем цветом */
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_fill(cr);
    
    cairo_destroy(cr);
    
    /* Создаём текстуру */
    return create_texture_from_cairo(mgr, parent, surface, size);
}

/* Однотонный прямоугольник-текстура (фейдится через opacity scene_buffer). */
struct wlr_scene_buffer *icon_create_solid(struct mywm_icon_manager *mgr,
                                           struct wlr_scene_tree *parent,
                                           int w, int h,
                                           const float color[4]) {
    if (!mgr || !parent || w <= 0 || h <= 0) {
        return NULL;
    }
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_paint(cr);
    cairo_destroy(cr);
    return create_texture_from_cairo_sized(mgr, parent, surface, w, h);
}

/* Иконка меню приложений: скруглённый квадрат с сеткой 3x3 точек. */
struct wlr_scene_buffer *icon_create_launchpad(struct mywm_icon_manager *mgr,
                                               struct wlr_scene_tree *parent,
                                               int size) {
    if (!mgr || !parent) {
        return NULL;
    }
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double r = size * 0.2;
    cairo_new_path(cr);
    cairo_arc(cr, r, r, r, M_PI, 3 * M_PI / 2);
    cairo_arc(cr, size - r, r, r, 3 * M_PI / 2, 2 * M_PI);
    cairo_arc(cr, size - r, size - r, r, 0, M_PI / 2);
    cairo_arc(cr, r, size - r, r, M_PI / 2, M_PI);
    cairo_close_path(cr);
    cairo_pattern_t *pat = cairo_pattern_create_linear(0, 0, 0, size);
    cairo_pattern_add_color_stop_rgba(pat, 0, 0.42, 0.45, 0.52, 1.0);
    cairo_pattern_add_color_stop_rgba(pat, 1, 0.16, 0.17, 0.22, 1.0);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    double c = size / 2.0;
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            cairo_new_path(cr);
            cairo_arc(cr, c + i * size * 0.24, c + j * size * 0.24,
                      size * 0.07, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
    cairo_destroy(cr);

    return create_texture_from_cairo(mgr, parent, surface, size);
}

/* Основная функция загрузки иконки приложения */
struct wlr_scene_buffer *icon_load_app(struct mywm_icon_manager *mgr,
                                        struct wlr_scene_tree *parent,
                                        const char *app_id,
                                        int size) {    if (!mgr || !parent || !app_id) {
        return NULL;
    }
    
    char *icon_path = NULL;
    cairo_surface_t *surface = NULL;
    
    /* Пытаемся найти PNG иконку */
    icon_path = find_icon_file(mgr->icon_theme, app_id, size, "png");
    
    if (!icon_path) {
        /* Пытаемся найти SVG (пока не поддерживается, но оставляем на будущее) */
        icon_path = find_icon_file(mgr->icon_theme, app_id, size, "svg");
    }
    
    if (!icon_path) {
        /* Ищем в pixmaps */
        icon_path = find_icon_in_pixmaps(app_id, "png");
    }
    
    if (!icon_path) {
        /* Ищем в pixmaps с расширением .svg */
        icon_path = find_icon_in_pixmaps(app_id, "svg");
    }
    
    if (icon_path) {
        surface = load_image_from_file(icon_path, size);
    }
    
    if (surface) {
        return create_texture_from_cairo(mgr, parent, surface, size);
    }
    
    /* Fallback: создаём цветную иконку */
    static const float fallback_colors[][4] = {
        {0.2f, 0.6f, 1.0f, 0.9f},  /* Голубой */
        {1.0f, 0.4f, 0.2f, 0.9f},  /* Оранжевый */
        {0.2f, 0.8f, 0.4f, 0.9f},  /* Зелёный */
        {0.8f, 0.2f, 0.6f, 0.9f},  /* Розовый */
    };
    
    /* Выбираем цвет на основе хэша app_id */
    unsigned hash = 0;
    for (const char *p = app_id; *p; p++) {
        hash = hash * 31 + (unsigned)*p;
    }
    
    const float *color = fallback_colors[hash % 4];
    return icon_create_fallback(mgr, parent, size, color);
}

/* Загрузка иконки из кэша GNOME */
struct wlr_scene_buffer *icon_load_from_gnome_cache(
    struct mywm_icon_manager *mgr,
    struct wlr_scene_tree *parent,
    const char *icon_name,
    int size) {
    
    /* GNOME хранит иконки в /usr/share/icons/gnome/ */
    /* Также использует кэш в ~/.cache/icon-cache.kcache */
    
    /* Пока просто используем стандартный поиск */
    return icon_load_app(mgr, parent, icon_name, size);
}

/* Получение списка доступных тем */
char **icon_get_available_themes(void) {
    char **themes = calloc(20, sizeof(char*));
    if (!themes) return NULL;
    
    int count = 0;
    DIR *dir;
    struct dirent *entry;
    
    /* Сканируем /usr/share/icons */
    dir = opendir("/usr/share/icons");
    if (dir) {
        while ((entry = readdir(dir)) != NULL && count < 18) {
            if (entry->d_type == DT_DIR && 
                entry->d_name[0] != '.') {
                themes[count++] = strdup(entry->d_name);
            }
        }
        closedir(dir);
    }
    
    themes[count] = NULL;
    return themes;
}

void icon_theme_list_free(char **themes) {
    if (!themes) return;
    
    for (int i = 0; themes[i] != NULL; i++) {
        free(themes[i]);
    }
    free(themes);
}
