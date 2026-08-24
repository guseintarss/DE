#ifndef ICONS_H
#define ICONS_H

#include <wlr/types/wlr_scene.h>
#include <wlr/render/gles2.h>
#include <cairo/cairo.h>
#include <fontconfig/fontconfig.h>

/* Контекст для управления иконками */
struct mywm_icon_manager {
    struct wlr_renderer *renderer;
    char *icon_theme;          /* Название темы иконок (например, "Adwaita") */
    int icon_size;             /* Размер иконок по умолчанию */
};

/* Инициализация менеджера иконок */
void icon_manager_init(struct mywm_icon_manager *mgr,
                       struct wlr_renderer *renderer);

/* Завершение работы менеджера иконок */
void icon_manager_finish(struct mywm_icon_manager *mgr);

/* 
 * Загрузка иконки приложения по app_id.
 * Ищет иконку в следующих местах:
 * 1. /usr/share/icons/<theme>/<size>/apps/<app_id>.png
 * 2. /usr/share/icons/<theme>/<size>/apps/<app_id>.svg
 * 3. /usr/share/pixmaps/<app_id>.png
 * 4.fallback иконка
 * 
 * Возвращает wlr_scene_buffer с загруженной текстурой или NULL при ошибке.
 */
struct wlr_scene_buffer *icon_load_app(struct mywm_icon_manager *mgr,
                                        struct wlr_scene_tree *parent,
                                        const char *app_id,
                                        int size);

/*
 * Загрузка иконки из стандартного кэша GNOME.
 * Использует gtk-update-icon-cache для поиска иконок.
 */
struct wlr_scene_buffer *icon_load_from_gnome_cache(
    struct mywm_icon_manager *mgr,
    struct wlr_scene_tree *parent,
    const char *icon_name,
    int size);

/*
 * Создание fallback иконки (простой цветной прямоугольник с закруглёнными углами).
 */
struct wlr_scene_buffer *icon_create_fallback(struct mywm_icon_manager *mgr,
                                               struct wlr_scene_tree *parent,
                                               int size,
                                               const float color[4]);

/*
 * Иконка меню приложений: скруглённый квадрат с сеткой 3x3 точек.
 */
struct wlr_scene_buffer *icon_create_launchpad(struct mywm_icon_manager *mgr,
                                               struct wlr_scene_tree *parent,
                                               int size);

/*
 * Однотонный прямоугольник-текстура (фейдится через opacity scene_buffer).
 */
struct wlr_scene_buffer *icon_create_solid(struct mywm_icon_manager *mgr,
                                           struct wlr_scene_tree *parent,
                                           int w, int h,
                                           const float color[4]);

/*
 * Получить список доступных тем иконок в системе.
 * Возвращает массив строк, завершённый NULL.
 * Освобождение: icon_theme_list_free().
 */
char **icon_get_available_themes(void);

/* Освобождение списка тем */
void icon_theme_list_free(char **themes);

#endif
