#ifndef SERVER_H
#define SERVER_H

#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "effects.h"
#include "wallpaper.h"
#include "icons.h"
#include <cairo.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
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

/* Количество слоёв zwlr-layer-shell (background, bottom, top, overlay). */
#define SHELL_LAYER_COUNT 4

/*
 * Поверхность layer-shell (waybar, AGSv2, QuickShell и т.п.). Описание
 * в src/layer_shell.c; списки хранит mywm_server.layer_lists.
 */
struct mywm_layer_surface;

/*
 * Хэндл окна для wlr-foreign-toplevel-management (список окон для
 * таскбаров/доков внешних оболочек). Описание в src/foreign_toplevel.c.
 */
struct mywm_ftl_toplevel;

/*
 * Рабочий стол (Spaces). Каждому столу соответствует scene-дерево —
 * ребёнок view_tree; окна стола живут в нём, видимость стола =
 * enabled его дерева. Описание в src/workspace.c.
 */
struct mywm_workspace {
    struct wl_list link;            /* server->ws.list, по возрастанию index */
    struct mywm_server *server;
    struct wlr_scene_tree *tree;
    int index;                      /* 0-based */
};

/* Состояние набора рабочих столов (server->ws). */
struct mywm_workspaces {
    struct wl_list list;            /* mywm_workspace::link, по возрастанию index */
    struct mywm_workspace *current;
    /* Анимация слайда при переключении. */
    struct wl_event_source *anim_timer;
    double progress;                /* 0..1 */
    int dir;                        /* +1 вправо, -1 влево, 0 — нет анимации */
    int width;                      /* ширина размаха (layout), px */
    struct mywm_workspace *from, *to;
};

/*
 * Менеджер блокировки сессии (ext-session-lock-v1) + idle-нотификатор.
 * Описание в src/session_lock.c.
 */
struct mywm_lock_manager;
struct wlr_idle_notifier_v1;

/* Невидимая хит-зона ресайза по краям окна. */
#define RESIZE_HIT 6
/* Отступ кнопок управления слева в заголовке. */
#define BTN_X 8
#define BTN_DOUBLE_CLICK_MS 400

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
    double vw;                     /* Скорость пружины размера */
    int lx, ly, lw, lh;
    /* Закреплённый лаунчер: виден всегда (даже без окон), клик без
     * запущенного приложения выполняет command. */
    bool pinned;
    char app_id[64];
    char command[128];
};

struct mywm_dock {
    struct mywm_server *server;
    struct wlr_scene_tree *tree;
    struct wlr_scene_rect *bar;
    struct wlr_scene_rect *sep;
    /* Разделитель между закреплёнными лаунчерами и окнами. */
    struct wlr_scene_rect *sep_pin;
    struct wlr_scene_rect *dot;
    struct wl_list items;
    struct wl_event_source *anim_timer;
    bool anim_running;             /* Идёт ли сейчас анимация дока */
    struct timespec anim_last;     /* Время прошлого тика (для dt) */
    /* Прямоугольник полосы дока в координатах layout (для fisheye). */
    int bar_x, bar_y, bar_w, bar_h;
};

/* CPU-буфер под текст (memfd + cairo), см. bar.c. */
#include <wlr/interfaces/wlr_buffer.h>
struct mywm_text_buf {
    struct wlr_buffer base;
    int fd;
    void *data;
    size_t size;
    size_t stride;
};
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

    /* Ввод ([input] в config.toml): тачпад и ускорение. */
    struct input_config input_cfg;

    /* Оболочка ([shell]): builtin=false отключает встроенные менюбар и
     * док (их место занимает внешняя оболочка через layer-shell). */
    struct shell_config shell_cfg;

    /* Дизайн оболочки ([design]): цвета/метрики менюбара, дока,
     * декораций окон. Применяется на лету по SIGHUP. */
    struct design_config design;

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
    /* Полноэкранное меню приложений (Launchpad), создаётся в apps_menu_init. */
    struct mywm_apps_menu *apps_menu;

    /* --- Внешняя оболочка: zwlr-layer-shell + foreign-toplevel --- */
    /* Глобальные деревья слоёв сцены. Порядок создания задаёт Z-порядок:
     * background < bottom < view_tree < top < overlay. */
    struct wlr_scene_tree *layer_trees[SHELL_LAYER_COUNT];
    /* Списки поверхностей по слоям (struct mywm_layer_surface::link). */
    struct wl_list layer_lists[SHELL_LAYER_COUNT];
    struct wlr_layer_shell_v1 *layer_shell;
    /* Дерево декораций всех xdg-окон (между bottom- и top-слоями). */
    struct wlr_scene_tree *view_tree;
    /* Суммарные эксклюзивные зоны layer-поверхностей по краям layout
     * (пересчитываются в arrange_layers). */
    int reserved_top, reserved_bottom, reserved_left, reserved_right;
    struct wlr_foreign_toplevel_manager_v1 *ftl_manager;
    struct wl_list ftl_toplevels;   /* struct mywm_ftl_toplevel::link */

    /* --- Рабочие столы (Spaces, src/workspace.c) --- */
    struct mywm_workspaces ws;

    /* --- Блокировка сессии и простой (src/session_lock.c) --- */
    struct mywm_lock_manager *lock;
    /* ext-idle-notify: нотификатор, кормится активностью из ввода. */
    struct wlr_idle_notifier_v1 *idle_notifier;

    struct wl_listener new_layer_surface;

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
    /* Курсор в начале MOVE-жеста: отличаем клик от перетаскивания
     * (edge-tiling срабатывает только при реальном переносе). */
    double grab_start_x, grab_start_y;
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
    /* Трёхпальцевый свайп по тачпаду — переключение рабочих столов. */
    struct wl_listener cursor_swipe_begin;
    struct wl_listener cursor_swipe_update;
    struct wl_listener cursor_swipe_end;
    /* Night light: применение LUT от wlr-gamma-control клиентов. */
    struct wl_listener gamma_set_gamma;
    bool ws_gesture_active;
    bool ws_gesture_done;          /* свайп уже вызвал переключение */
    double ws_gesture_dx;          /* накопленный горизонтальный сдвиг */
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
    /* Мягкая тень под окном (общая текстура, растягивается dest_size). */
    struct wlr_scene_buffer *shadow;
    /* Заголовок окна по центру тайтлбара (белый текст, shell_label_buf). */
    struct wlr_scene_buffer *title_node;
    struct mywm_text_buf *title_buf;
    char *title_cache;
    /* Круглые кнопки в заголовке: 0=CLOSE, 1=MINIMIZE, 2=MAXIMIZE. */
    struct mywm_btn btns[3];
    /* Состояние окна: свёрнуто (узел отключён) / развёрнуто на весь layout. */
    bool minimized;
    bool maximized;
    /* GNOME-тайлинг: 0 — нет, 1 — левая половина, 2 — правая. */
    int tiled_side;
    /* dock_resolve_view выполнен (app_id получен, иконка/пин привязаны). */
    bool dock_resolved;
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
    /* Рабочий стол (Spaces), на котором живёт окно. */
    struct mywm_workspace *ws;
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
    struct spring_anim spr_scale;   /* Масштаб открытия/закрытия */
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
    /* Время трансформации (мс): прогресс = ease(elapsed/duration),
     * вместо физической пружины — без перелёта геометрии. */
    double tform_elapsed;
    double tform_duration;
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
/* GNOME-тайлинг: side 1=левая половина, 2=правая, 0=вернуть как было. */
void tile_view(struct mywm_view *view, int side);
/* Финальное освобождение view (вызывается после анимации закрытия). */
void view_destroy_final(struct mywm_view *view);

/* --- wallpaper.c --- */
/* (wallpaper_apply объявлен в wallpaper.h) */

/* --- dock.c --- */
void dock_init(struct mywm_server *server);
void dock_add_view(struct mywm_server *server, struct mywm_view *view);
/* Привязка окна к пину / догрузка иконки, когда клиент прислал app_id. */
void dock_resolve_view(struct mywm_server *server, struct mywm_view *view,
                       const char *app_id);
void dock_remove_view(struct mywm_server *server, struct mywm_view *view);
void dock_refresh(struct mywm_server *server);
void dock_update(struct mywm_server *server);
struct mywm_view *dock_icon_at(struct mywm_server *server,
                               double lx, double ly);
/* Клик по дока-иконке: фокус существующего окна либо запуск закреплённого
 * лаунчера. Возвращает true, если клик поглощён доком. */
bool dock_activate_at(struct mywm_server *server, double lx, double ly);
void dock_raise(struct mywm_server *server);
/* Повторное применение [design] к доку. */
void dock_redesign(struct mywm_server *server);

/* --- keyboard.c --- */
/* Запуск команды через fork/execvp (без шелла). */
void mywm_spawn(struct mywm_server *server, const char *command);

/* --- apps_menu.c --- */
void apps_menu_init(struct mywm_server *server);
void apps_menu_toggle(struct mywm_server *server);
bool apps_menu_is_open(const struct mywm_server *server);
void apps_menu_scroll(struct mywm_server *server, double delta,
                      uint32_t source);
/* Клик при открытом меню: запуск приложения под курсором либо закрытие.
 * Всегда поглощает клик (возвращает true). */
bool apps_menu_click(struct mywm_server *server, double lx, double ly);
void apps_menu_motion(struct mywm_server *server, double lx, double ly);
/* Дерево меню в сцене (NULL, если меню не создано). */
struct wlr_scene_tree *apps_menu_tree(struct mywm_server *server);

/* --- bar.c --- */
/* Текстовый буфер для подписей оболочки (меню приложений и т.п.). */
struct mywm_text_buf *shell_label_buf(struct mywm_server *server,
                                      const char *text, int px);
struct mywm_text_buf *shell_blank_buf(int width, int height);
struct mywm_text_buf *shell_label_buf_weight(struct mywm_server *server,
                                             const char *text, int px,
                                             cairo_font_weight_t weight);

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
struct wlr_scene_buffer *icon_create_launchpad(struct mywm_icon_manager *mgr,
                                               struct wlr_scene_tree *parent,
                                               int size);
struct wlr_scene_buffer *icon_create_solid(struct mywm_icon_manager *mgr,
                                           struct wlr_scene_tree *parent,
                                           int w, int h,
                                           const float color[4]);
char **icon_get_available_themes(void);
void icon_theme_list_free(char **themes);

/* --- bar.c --- */
void bar_init(struct mywm_server *server);
void bar_update_name(struct mywm_server *server);
void bar_raise(struct mywm_server *server);
/* Повторное применение [design] к менюбару (цвета, кнопки, тексты). */
void bar_redesign(struct mywm_server *server);
/* Какая кнопка максимизированного окна в менюбаре под точкой. */
enum mywm_title_button bar_button_at(struct mywm_server *server,
                                     double lx, double ly);
/* Круглая кнопка: буфер с закрашенным кругом и (опционально) глифом. */
struct mywm_text_buf *mywm_button_buf(int size, const float color[4],
                                      enum mywm_title_button glyph);
struct mywm_text_buf *mywm_button_buf_fg(int size, const float bg[4],
                                         enum mywm_title_button glyph,
                                         const float fg[4]);
/* Полный набор кнопки: узел + оба буфера, начальное состояние — круг.
 * Вариант _h позволяет задать фон/цвет глифа при наведении (Adwaita). */
struct mywm_btn mywm_btn_create(struct wlr_scene_tree *parent, int size,
                                const float color[4],
                                enum mywm_title_button glyph_btn);
struct mywm_btn mywm_btn_create_h(struct wlr_scene_tree *parent, int size,
                                  const float color[4],
                                  enum mywm_title_button glyph_btn,
                                  const float hover_bg[4],
                                  const float hover_fg[4]);
/* Переключает узел кнопки между глифом и обычным кругом. */
void mywm_button_hover(struct mywm_btn *btn, bool hovered);
/* Пересоздаёт кнопку с новыми цветом/размером ([design] reload).
 * Позицию и enabled выставляет вызывающий. */
void mywm_btn_recreate(struct wlr_scene_tree *parent, struct mywm_btn *btn,
                       int size, const float color[4],
                       enum mywm_title_button glyph_btn);

/* --- design.c --- */
/* Применяет server->design ко всем живым нодам: декорации окон, хром,
 * кнопки, менюбар, док. Вызывается после config_design_reload (SIGHUP). */
void design_apply(struct mywm_server *server);

/* --- layer_shell.c --- */
/* Глобальные деревья слоёв, менеджер zwlr-layer-shell, обработка новых
 * layer-поверхностей. Вызывать сразу после создания сцены. */
void layer_shell_init(struct mywm_server *server);
/* Пересобрать расположение всех layer-поверхностей и полезную область
 * (вызывается при map/unmap/commit слоёв и появлении выхода). */
void arrange_layers(struct mywm_server *server);
/* Полезная область layout: layout минус эксклюзивные зоны слоёв и
 * встроенный менюбар (если [shell].builtin). */
struct wlr_box shell_usable_box(struct mywm_server *server);
/* Пере-разместить максимизированные/тайленные окна под новую полезную
 * область. Вызывается после её изменения. */
void shell_relayout(struct mywm_server *server);

/* --- foreign_toplevel.c --- */
void foreign_toplevel_init(struct mywm_server *server);
/* Жизненный цикл хэндла окна: создание при map, закрытие при unmap/
 * destroy, обновление title/app_id/state по мере изменения. */
void foreign_toplevel_map(struct mywm_view *view);
void foreign_toplevel_unmap(struct mywm_view *view);
void foreign_toplevel_title(struct mywm_view *view);
void foreign_toplevel_state(struct mywm_view *view);

/* --- session_lock.c --- */
/* ext-session-lock-v1, wlr-data-control, ext-idle-notify + inhibit.
 * Вызывать в самом конце server_init (дерево лок-поверхностей должно
 * создаться выше всех остальных). */
void session_lock_init(struct mywm_server *server);
/* Сессия заблокирована: биндинги/клики оболочки отключены. */
bool session_lock_active(const struct mywm_server *server);
/* Сообщить нотификатору простоя о пользовательской активности. */
void idle_notify_activity(struct mywm_server *server);

/* --- workspace.c --- */
/* Создать воркспейс 0; вызывать после layer_shell_init (view_tree готов). */
void workspaces_init(struct mywm_server *server);
/* Дерево активного стола — родитель новых окон. */
struct wlr_scene_tree *workspace_active_tree(struct mywm_server *server);
/* Переключение (со слайдом); index 0-based, стол создаётся при need. */
void workspace_switch(struct mywm_server *server, int index);
void workspace_switch_next(struct mywm_server *server);
void workspace_switch_prev(struct mywm_server *server);
/* Перенести окно на стол index (0-based) и переключиться за ним. */
void workspace_move_view(struct mywm_view *view, int index);
void workspace_move_view_next(struct mywm_view *view);
void workspace_move_view_prev(struct mywm_view *view);
/* Окно принадлежит активному столу (для alt-tab и рендера). */
bool workspace_view_visible(const struct mywm_view *view);
int workspace_current_index(const struct mywm_server *server);
int workspace_count(const struct mywm_server *server);

#endif