#include "icons.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <cairo/cairo.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/allocator.h>
#include <wlr/render/renderer.h>
#include <wlr/render/swapchain.h>
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
                       struct wlr_renderer *renderer,
                       struct wlr_allocator *allocator) {
    if (!mgr) return;
    
    mgr->renderer = renderer;
    mgr->allocator = allocator;
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

/* Вспомогательная функция для поиска файла иконки */
static char *find_icon_file(const char *theme, const char *icon_name, 
                            int size, const char *extension) {
    static char path[1024];
    char size_str[32];
    
    /* Определяем размерную категорию */
    if (size <= 16) snprintf(size_str, sizeof(size_str), "16x16");
    else if (size <= 24) snprintf(size_str, sizeof(size_str), "24x24");
    else if (size <= 32) snprintf(size_str, sizeof(size_str), "32x32");
    else if (size <= 48) snprintf(size_str, sizeof(size_str), "48x48");
    else if (size <= 64) snprintf(size_str, sizeof(size_str), "64x64");
    else if (size <= 96) snprintf(size_str, sizeof(size_str), "96x96");
    else if (size <= 128) snprintf(size_str, sizeof(size_str), "128x128");
    else snprintf(size_str, sizeof(size_str), "scalable");
    
    /* Путь в теме иконок */
    snprintf(path, sizeof(path), 
             "/usr/share/icons/%s/%s/apps/%s.%s",
             theme, size_str, icon_name, extension);
    
    if (access(path, R_OK) == 0) {
        return path;
    }
    
    /* Пробуем без поддиректории apps */
    snprintf(path, sizeof(path), 
             "/usr/share/icons/%s/%s/%s.%s",
             theme, size_str, icon_name, extension);
    
    if (access(path, R_OK) == 0) {
        return path;
    }
    
    /* Пробуем scalable */
    snprintf(path, sizeof(path), 
             "/usr/share/icons/%s/scalable/apps/%s.%s",
             theme, icon_name, extension);
    
    if (access(path, R_OK) == 0) {
        return path;
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
static cairo_surface_t *load_image_from_file(const char *filename, int size) {
    cairo_surface_t *surface = NULL;
    
    /* Определяем формат по расширению */
    const char *ext = strrchr(filename, '.');
    if (!ext) return NULL;
    
    if (strcasecmp(ext, ".png") == 0) {
        surface = cairo_image_surface_create_from_png(filename);
    } else if (strcasecmp(ext, ".svg") == 0) {
        /* Для SVG потребуется librsvg, пока возвращаем NULL */
        wlr_log(WLR_DEBUG, "SVG support requires librsvg: %s", filename);
        return NULL;
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

/* Создание текстуры wlroots из Cairo поверхности */
static struct wlr_scene_buffer *create_texture_from_cairo(
    struct mywm_icon_manager *mgr,
    struct wlr_scene_tree *parent,
    cairo_surface_t *surface,
    int size) {
    
    if (!surface || !mgr || !parent) {
        return NULL;
    }
    
    int width = cairo_image_surface_get_width(surface);
    int height = cairo_image_surface_get_height(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    
    if (!data) {
        return NULL;
    }
    
    /* Создаем wlr_buffer из данных */
    struct wlr_buffer *wlr_buf = wlr_buffer_alloc(width, height);
    if (!wlr_buf) {
        return NULL;
    }
    
    /* Копируем данные пикселей */
    struct wlr_client_buffer *client_buf = wlr_client_buffer_create(
        wlr_buf, mgr->renderer);
    
    if (!client_buf) {
        wlr_buffer_drop(wlr_buf);
        return NULL;
    }
    
    /* Создаём scene_buffer */
    struct wlr_scene_buffer *scene_buf = wlr_scene_buffer_create(parent, client_buf);
    if (!scene_buf) {
        wlr_buffer_drop(wlr_buf);
        return NULL;
    }
    
    /* Устанавливаем размер */
    wlr_scene_buffer_set_dest_size(scene_buf, size, size);
    
    /* Освобождаем Cairo поверхность */
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    
    return scene_buf;
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

/* Основная функция загрузки иконки приложения */
struct wlr_scene_buffer *icon_load_app(struct mywm_icon_manager *mgr,
                                        struct wlr_scene_tree *parent,
                                        const char *app_id,
                                        int size) {
    if (!mgr || !parent || !app_id) {
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
