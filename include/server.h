#ifndef SERVER_H
#define SERVER_H

#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "effects.h"
#include "wallpaper.h"
#include "icons.h"
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>

enum mywm_cursor_mode {
    MYWM_CURSOR_PASSTHROUGH,
    MYWM_CURSOR_MOVE,
    MYWM_CURSOR_RESIZE,
};

/* Декорации окна: граница и заголовок (в пикселях). */
#define DECO_BORDER 4
#define DECO_TITLE 28
/* Невидимая хит-зона ресайза по краям окна. */
#define RESIZE_HIT 6
/* Кнопки управления на заголовке (как в macOS: красная/жёлтая/зелёная). */
#define BTN_SIZE 11
#define BTN_GAP 6
#define BTN_X 8
#define BTN_Y ((DECO_TITLE - BTN_SIZE) / 2)
#define BTN_DOUBLE_CLICK_MS 400
/* Верхний менюбар: максимизированные окна начинаются ниже него. */
#define MENU_BAR_HEIGHT 26

enum mywm_title_button {
    MYWM_BTN_NONE,
    MYWM_BTN_CLOSE,
    MYWM_BTN_MINIMIZE,
    MYWM_BTN_MAXIMIZE,
};

/* Круглая кнопка как в macOS: узел + буферы (обычный и с глифом при
 * наведении). Порядок в btns[]: 0=CLOSE, 1=MINIMIZE, 2=MAXIMIZE. */
struct mywm_btn {
    struct wlr_scene_buffer *node;
    struct mywm_text_buf *plain;
    struct mywm_text_buf *glyph;
};

#define BTN_INDEX(btn) ((int)(btn) - 1)

struct mywm_dock_item {
    struct wl_list link;
    struct mywm_view *view;
    struct wlr_scene_rect *icon;      /* Прямоугольник (fallback) */
    struct wlr_scene_buffer *icon_img; /* Текстура с иконкой приложения */
    /* Текущая (анимированная) и целевая геометрия в координатах
     * output layout. */
    double cw, ch, tw, th;
    int lx, ly, lw, lh;
};

struct mywm_dock {
    struct mywm_server *server;
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *bar;
    struct wlr_scene_rect *sep;
    struct wlr_scene_rect *dot;
    struct wl_list items;
    struct wl_event_source *anim_timer;
};

struct mywm_text_buf;
struct mywm_chrome_buf;

struct mywm_bar {
    struct mywm_server *server;
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *bg;
    struct wlr_scene_rect *line;
    struct wlr_scene_buffer *app;
    struct wlr_scene_buffer *menus;
    struct wlr_scene_buffer *clock;
    /* Собственные ссылки на буферы: wlroots освобождает буфер после
     * загрузки текстуры (scene_buffer->buffer становится NULL), а ширина
     * нужна для раскладки. */
    struct mywm_text_buf *app_buf;
    struct mywm_text_buf *menus_buf;
    struct mywm_text_buf *clock_buf;
    /* Иконки статуса справа (WiFi, батарея). */
    struct wlr_scene_buffer *wifi;
    struct wlr_scene_buffer *battery;
    struct mywm_text_buf *wifi_buf;
    struct mywm_text_buf *battery_buf;
    struct wl_event_source *clock_timer;
    /* Кнопки управления в баре: 0=CLOSE, 1=MINIMIZE, 2=MAXIMIZE. */
    struct mywm_btn btns[3];
};

struct mywm_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_compositor *compositor;
    struct wlr_output_layout *output_layout;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_scene *scene;

    struct mywm_wallpaper *wallpaper;

    /* Менеджер иконок приложений */
    struct mywm_icon_manager icon_mgr;

    /* Конфигурация этапа 5 ([wallpaper]/[animations] в config.toml). */
    struct wallpaper_config wallpaper_cfg;
    struct animations_config animations_cfg;

    /* Общий XKB-контекст для всех клавиатур (создаётся один раз). */
    struct xkb_context *xkb_context;

    /* Таблица горячих клавиш, заполняется config_load_defaults +
     * config_load (переопределения из config.toml). */
    struct keybinding bindings[MAX_BINDINGS];
    size_t bindings_len;
    /* Раскладка XKB из конфига (NULL — системная по умолчанию). */
    char *keyboard_layout;

    /* Флаг выхода: ставится биндингом BIND_ACTION_EXIT, проверяется
     * циклом в server_run. */
    bool terminate;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_seat *seat;

    struct mywm_dock dock;
    struct mywm_bar bar;

    struct wl_list outputs;
    struct wl_list views;
    struct wl_list keyboards;

    struct mywm_view *focused_view;
    /* View под курсором с подсветкой рамки (hover-эффект). */
    struct mywm_view *hovered_view;

    /* Единый глобальный таймер spring-анимаций (animation.c). */
    struct wl_event_source *anim_timer;
    /* Время последнего тика анимации (dt между кадрами). */
    struct timespec anim_last;

    /* Состояние интерактивной операции (move/resize).
     * grab_geobox хранит исходную геометрию view (x, y, width, height)
     * в координатах layout на момент начала жеста. */
    enum mywm_cursor_mode cursor_mode;
    struct mywm_view *grabbed_view;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    bool mod_pressed;

    /* Для детекции двойного клика по заголовку (время и позиция). */
    uint32_t last_press_time;
    double last_press_x, last_press_y;

    struct wl_listener new_output;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_listener new_input;
    struct wl_listener new_virtual_keyboard;
    struct wl_listener new_virtual_pointer;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_cursor;
};

struct mywm_output {
    struct wl_list link;
    struct mywm_server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    /* Субдерево обоев — самый нижний слой сцены (bg_tree -> background). */
    struct wlr_scene_tree *bg_tree;
    struct wlr_scene_buffer *background;
    struct wlr_scene_rect *background_fallback;
    /* Тайлы для WALLPAPER_MODE_TILE. */
    struct wlr_scene_buffer *tiles[16];
    int tile_count;
    struct wl_listener frame;
    struct wl_listener destroy;
};

struct mywm_view {
    /* view->link включается в server->views ровно один раз, при создании
     * view (new_toplevel), и удаляется ровно один раз, в destroy. В map/unmap
     * список не трогается; для фильтрации видимости используется view->mapped. */
    struct wl_list link;
    struct mywm_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    /* Контейнер декораций (рамка + заголовок). scene_tree — его ребёнок,
     * поэтому move/focus/raise двигают всё окно вместе. */
    struct wlr_scene_tree *deco_tree;
    /* Декорации: граница, тело окна, заголовок. */
    struct wlr_scene_rect *deco_border;
    struct wlr_scene_rect *deco_body;
    struct wlr_scene_rect *deco_title;
    /* Хром окна: одна CPU-текстура со скруглёнными углами (effects.c).
     * chrome_buf — собственный lock буфера; chrome_w/h — размер текстуры. */
    struct wlr_scene_buffer *chrome;
    struct mywm_chrome_buf *chrome_buf;
    int chrome_w, chrome_h;
    /* Круглые кнопки в заголовке: 0=CLOSE, 1=MINIMIZE, 2=MAXIMIZE. */
    struct mywm_btn btns[3];
    /* Состояние окна: свёрнуто (узел отключён) / развёрнуто на весь layout. */
    bool minimized;
    bool maximized;
    /* Геометрия до максимизации (deco-позиция и размер содержимого). */
    int save_x, save_y, save_w, save_h;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    /* wlroots уничтожает scene_tree сам при destroy поверхности
     * (wlr_scene_xdg_surface); этот listener обнуляет указатели, чтобы
     * тик анимации не трогал освобождённую память. */
    struct wl_listener scene_destroy;
    bool mapped;
    /* Позиция view в координатах wlr_output_layout.
     * x/y обновляются композитором при перемещении и синхронизируются
     * с позицией scene-ноды в commit. */
    int x, y;
    /* Текущий размер поверхности, подтверждённый клиентом (из commit). */
    int width, height;

    /* --- Этап 5: эффекты --- */
    /* Scene-буфер содержимого клиента (первый buffer-child scene_tree). */
    struct wlr_scene_buffer *content_buffer;
    /* Spring-анимации: opacity, slide (магнит к границам), hover. */
    struct spring_anim spr_opacity;
    struct spring_anim spr_slide;
    struct spring_anim spr_hover;
    bool anim_active;
    /* Анимация закрытия в процессе (финальный destroy отложен). */
    bool closing;
    /* Курсор над заголовком окна (подсветка). */
    bool hovered;
    /* Время последнего тика анимации (dt между кадрами). */
    struct timespec anim_last;

    /* --- Трансформации окна (maximize/unmaximize/genie) --- */
    bool tform_active;
    enum mywm_tform_kind tform_kind;
    struct spring_anim tform_spr;
    struct tform_geo tform_a, tform_b;
    /* Исходная геометрия до genie и позиция иконки дока (для обратного
     * движения). */
    struct tform_geo tform_home, tform_min_geo;
};

struct mywm_keyboard {
    struct wl_list link;
    struct mywm_server *server;
    struct wlr_keyboard *wlr_keyboard;
    /* Отслеживание автоповтора: последний keycode и время его PRESSED. */
    uint32_t last_repeat_keycode;
    uint32_t last_repeat_time;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

struct mywm_popup {
    struct mywm_server *server;
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

void server_init(struct mywm_server *server);
void server_run(struct mywm_server *server);
void server_finish(struct mywm_server *server);

/* --- cursor.c --- */
void cursor_init(struct mywm_server *server);
void server_new_pointer(struct mywm_server *server,
                        struct wlr_input_device *device);
void begin_interactive(struct mywm_view *view,
                       enum mywm_cursor_mode mode, uint32_t edges);
void reset_cursor_mode(struct mywm_server *server);
void clamp_view_to_layout(struct mywm_server *server, struct mywm_view *view,
                          int *x, int *y);

/* --- keyboard.c --- */
void server_new_keyboard(struct mywm_server *server,
                         struct wlr_keyboard *wlr_keyboard);

/* --- xdg_shell.c --- */
void xdg_shell_init(struct mywm_server *server);
void focus_view(struct mywm_server *server, struct mywm_view *view,
                struct wlr_surface *surface);
void close_view(struct mywm_view *view);
void minimize_view(struct mywm_view *view);
void maximize_view(struct mywm_view *view);
/* Финальное освобождение view (вызывается после анимации закрытия). */
void view_destroy_final(struct mywm_view *view);

/* --- wallpaper.c --- */
/* (wallpaper_apply объявлен в wallpaper.h) */

/* --- dock.c --- */
void dock_init(struct mywm_server *server);
void dock_add_view(struct mywm_server *server, struct mywm_view *view);
void dock_remove_view(struct mywm_server *server, struct mywm_view *view);
void dock_refresh(struct mywm_server *server);
void dock_update(struct mywm_server *server);
struct mywm_view *dock_icon_at(struct mywm_server *server,
                               double lx, double ly);
void dock_raise(struct mywm_server *server);

/* --- icons.c --- */
void icon_manager_init(struct mywm_icon_manager *mgr,
                       struct wlr_renderer *renderer);
void icon_manager_finish(struct mywm_icon_manager *mgr);
struct wlr_scene_buffer *icon_load_app(struct mywm_icon_manager *mgr,
                                        struct wlr_scene_tree *parent,
                                        const char *app_id,
                                        int size);
struct wlr_scene_buffer *icon_load_from_gnome_cache(
    struct mywm_icon_manager *mgr,
    struct wlr_scene_tree *parent,
    const char *icon_name,
    int size);
struct wlr_scene_buffer *icon_create_fallback(struct mywm_icon_manager *mgr,
                                               struct wlr_scene_tree *parent,
                                               int size,
                                               const float color[4]);
char **icon_get_available_themes(void);
void icon_theme_list_free(char **themes);

/* --- bar.c --- */
void bar_init(struct mywm_server *server);
void bar_update_name(struct mywm_server *server);
void bar_raise(struct mywm_server *server);
/* Круглая кнопка: буфер с закрашенным кругом и (опционально) глифом. */
struct mywm_text_buf *mywm_button_buf(int size, const float color[4],
                                      enum mywm_title_button glyph);
/* Полный набор кнопки: узел + оба буфера, начальное состояние — круг. */
struct mywm_btn mywm_btn_create(struct wlr_scene_tree *parent, int size,
                                const float color[4],
                                enum mywm_title_button glyph_btn);
/* Переключает узел кнопки между глифом и обычным кругом. */
void mywm_button_hover(struct mywm_btn *btn, bool hovered);

#endif