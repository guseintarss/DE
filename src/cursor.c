#include "server.h"
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

static struct mywm_view *desktop_view_at(struct mywm_server *server,
        double lx, double ly, struct wlr_surface **surface,
        double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }
    struct wlr_scene_buffer *scene_buffer = (struct wlr_scene_buffer *)node;
    struct wlr_scene_surface *scene_surface =
        wlr_scene_surface_try_from_buffer(scene_buffer);
    if (scene_surface == NULL) {
        return NULL;
    }
    *surface = scene_surface->surface;
    struct wlr_scene_tree *tree = ((struct wlr_scene_node *)scene_buffer)->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }
    return tree != NULL ? tree->node.data : NULL;
}

/*
 * Хит-тест по декорациям: если точка попала в рамку/заголовок какого-либо
 * окна (но не в содержимое), возвращаем view без поверхности — клик по
 * заголовку переключает фокус и стартует перенос.
 */
static struct mywm_view *desktop_deco_at(struct mywm_server *server,
        double lx, double ly) {
    const struct design_config *d = &server->design;
    struct mywm_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->mapped || view->minimized) {
            continue;
        }
        if (lx >= view->x && lx < view->x + view->width + 2 * d->border &&
                ly >= view->y && ly < view->y + view->height +
                    d->title_h + 2 * d->border) {
            return view;
        }
    }
    return NULL;
}

/* Попала ли точка в полосу заголовка окна. */
static bool point_in_title(struct mywm_view *view, double lx, double ly) {
    const struct design_config *d = &view->server->design;
    return lx >= view->x + d->border &&
        lx < view->x + view->width + d->border &&
        ly >= view->y && ly < view->y + d->title_h;
}

/* Какая кнопка управления в заголовке под точкой (MYWM_BTN_NONE — мимо). */
static enum mywm_title_button title_button_at(struct mywm_view *view,
        double lx, double ly) {
    const struct design_config *d = &view->server->design;
    int btn_y = (d->title_h - d->btn_size) / 2;
    double rx = lx - view->x - d->border;
    double ry = ly - view->y;
    if (ry < btn_y || ry >= btn_y + d->btn_size || rx < BTN_X) {
        return MYWM_BTN_NONE;
    }
    double btn = rx - BTN_X;
    if (btn < d->btn_size) {
        return MYWM_BTN_CLOSE;
    }
    if (btn < d->btn_size + d->btn_gap) {
        return MYWM_BTN_NONE;
    }
    if (btn < 2 * d->btn_size + d->btn_gap) {
        return MYWM_BTN_MINIMIZE;
    }
    if (btn < 2 * d->btn_size + 2 * d->btn_gap) {
        return MYWM_BTN_NONE;
    }
    if (btn < 3 * d->btn_size + 2 * d->btn_gap) {
        return MYWM_BTN_MAXIMIZE;
    }
    return MYWM_BTN_NONE;
}
/* Какие края окна (WLR_EDGE_*) находятся в хит-зоне ресайза у точки. */
static uint32_t view_resize_edges(struct mywm_view *view, double lx, double ly) {
    if (view->maximized) {
        return 0;
    }
    const struct design_config *d = &view->server->design;
    double left = lx - view->x;
    double right = view->x + view->width + 2 * d->border - lx;
    double top = ly - view->y;
    double bottom = view->y + view->height + d->title_h +
        2 * d->border - ly;
    uint32_t edges = 0;
    if (left < RESIZE_HIT) {
        edges |= WLR_EDGE_LEFT;
    }
    if (right < RESIZE_HIT) {
        edges |= WLR_EDGE_RIGHT;
    }
    if (top < RESIZE_HIT) {
        edges |= WLR_EDGE_TOP;
    }
    if (bottom < RESIZE_HIT) {
        edges |= WLR_EDGE_BOTTOM;
    }
    return edges;
}

static const char *resize_cursor_name(uint32_t edges) {
    switch (edges) {
    case WLR_EDGE_TOP:
        return "n-resize";
    case WLR_EDGE_BOTTOM:
        return "s-resize";
    case WLR_EDGE_LEFT:
        return "w-resize";
    case WLR_EDGE_RIGHT:
        return "e-resize";
    case WLR_EDGE_TOP | WLR_EDGE_LEFT:
        return "nw-resize";
    case WLR_EDGE_TOP | WLR_EDGE_RIGHT:
        return "ne-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_LEFT:
        return "sw-resize";
    case WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT:
        return "se-resize";
    default:
        return "default";
    }
}

void reset_cursor_mode(struct mywm_server *server) {
    server->cursor_mode = MYWM_CURSOR_PASSTHROUGH;
    server->grabbed_view = NULL;
}

/*
 * Ограничивает позицию окна границами wlr_output_layout, чтобы окно
 * нельзя было утащить в невидимую область (за края всех мониторов).
 * Окно всегда остаётся частично видимым (с отступом в margin пикселей).
 */
void clamp_view_to_layout(struct mywm_server *server, struct mywm_view *view,
                          int *x, int *y) {
    struct wlr_box layout_box;
    wlr_output_layout_get_box(server->output_layout, NULL, &layout_box);
    if (wlr_box_empty(&layout_box)) {
        return;
    }
    const struct design_config *d = &server->design;
    const int margin = 20;

    int min_x = layout_box.x + margin;
    int max_x = layout_box.x + layout_box.width - margin -
        (int)(view->width + 2 * d->border);
    int min_y = layout_box.y + margin;
    int max_y = layout_box.y + layout_box.height - margin -
        (int)(view->height + d->title_h + 2 * d->border);

    /* Окно шире/выше layout: прижимаем левый/верхний край к началу layout. */
    if (min_x > max_x) {
        min_x = max_x = layout_box.x;
    }
    if (min_y > max_y) {
        min_y = max_y = layout_box.y;
    }
    if (*x < min_x) {
        *x = min_x;
    }
    if (*x > max_x) {
        *x = max_x;
    }
    if (*y < min_y) {
        *y = min_y;
    }
    if (*y > max_y) {
        *y = max_y;
    }
}

static void process_cursor_move(struct mywm_server *server) {
    struct mywm_view *view = server->grabbed_view;
    if (view == NULL || !view->mapped) {
        return;
    }
    int new_x = (int)(server->cursor->x - server->grab_x);
    int new_y = (int)(server->cursor->y - server->grab_y);
    clamp_view_to_layout(server, view, &new_x, &new_y);
    view->x = new_x;
    view->y = new_y;
    effects_view_set_position(view, new_x, new_y);
    wlr_log(WLR_DEBUG, "cursor move: view=%p pos=(%d,%d)",
            (void *)view, new_x, new_y);
}

/*
 * Resize: пересчитываем геометрию от исходного grab_geobox по движению курсора.
 * Правые/нижние границы меняют размер через wlr_xdg_toplevel_set_size
 * (клиент получает configure и подтверждает новым буфером в commit),
 * левые/верхние — сдвигают scene-ноду на стороне композитора.
 */
static void process_cursor_resize(struct mywm_server *server) {
    struct mywm_view *view = server->grabbed_view;
    if (view == NULL || !view->mapped) {
        return;
    }
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x;
    int new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y;
    int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = (int)border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = (int)border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = (int)border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = (int)border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }


    wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                              new_right - new_left, new_bottom - new_top);
    wlr_log(WLR_DEBUG, "cursor resize: view=%p size=%dx%d",
            (void *)view, new_right - new_left, new_bottom - new_top);
}

/* Глифы кнопок при наведении (оконных и в менюбаре). */
static void update_buttons_hover(struct mywm_server *server) {
    double cx = server->cursor->x;
    double cy = server->cursor->y;
    struct mywm_view *view = server->focused_view;
    for (int i = 0; i < 3; i++) {
        if (view == NULL || view->btns[i].node == NULL) {
            continue;
        }
        bool over = !view->maximized &&
            title_button_at(view, cx, cy) ==
                (enum mywm_title_button)(i + 1);
        mywm_button_hover(&view->btns[i], over);
    }
    for (int i = 0; i < 3; i++) {
        bool over = bar_button_at(server, cx, cy) ==
            (enum mywm_title_button)(i + 1);
        mywm_button_hover(&server->bar.btns[i], over);
    }
}

static void process_cursor_motion(struct mywm_server *server, uint32_t time) {
    if (server->cursor_mode == MYWM_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == MYWM_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    /* Открытое меню приложений поглощает ввод: ховер по его ячейкам,
     * окна под оверлеем не получают pointer-события. */
    if (apps_menu_is_open(server)) {
        apps_menu_motion(server, server->cursor->x, server->cursor->y);
        wlr_seat_pointer_clear_focus(seat);
        return;
    }
    struct mywm_view *view = desktop_view_at(server,
            server->cursor->x, server->cursor->y, &surface, &sx, &sy);
    if (view == NULL) {
        view = desktop_deco_at(server, server->cursor->x, server->cursor->y);
    }
    if (view == NULL || surface == NULL) {
        uint32_t edges = view == NULL ? 0 :
            view_resize_edges(view, server->cursor->x, server->cursor->y);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
                               resize_cursor_name(edges));
    }
    /* Hover-подсветка рамки при наведении на заголовок окна. */
    struct mywm_view *hover_view = view;
    if (hover_view != NULL &&
            !point_in_title(hover_view, server->cursor->x, server->cursor->y)) {
        hover_view = NULL;
    }
    if (server->hovered_view != hover_view) {
        if (server->hovered_view != NULL) {
            view_effects_hover(server->hovered_view, false);
        }
        if (hover_view != NULL) {
            view_effects_hover(hover_view, true);
        }
        server->hovered_view = hover_view;
    }
    if (surface != NULL) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
    dock_update(server);
    update_buttons_hover(server);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    if (event == NULL) {
        return;
    }
    wlr_cursor_move(server->cursor, &event->pointer->base,
                    event->delta_x, event->delta_y);
    idle_notify_activity(server);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct mywm_server *server =
        wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    if (event == NULL) {
        return;
    }
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
                             event->x, event->y);
    idle_notify_activity(server);
    process_cursor_motion(server, event->time_msec);
}

/*
 * Старт интерактивной операции (move/resize). Запоминаем точку захвата
 * в координатах layout и исходную геометрию view для последующего расчёта.
 */
void begin_interactive(struct mywm_view *view,
                       enum mywm_cursor_mode mode, uint32_t edges) {
    if (view == NULL || !view->mapped || view->server->cursor == NULL) {
        return;
    }
    struct mywm_server *server = view->server;
    server->grabbed_view = view;
    server->cursor_mode = mode;

    if (mode == MYWM_CURSOR_MOVE) {
        /* Захват относительно внешнего края окна (декораций): точка нажатия
         * остаётся под курсором, окно не прыгает (как в Windows/macOS). */
        int deco_x, deco_y;
        wlr_scene_node_coords(&view->deco_tree->node, &deco_x, &deco_y);
        server->grab_x = server->cursor->x - deco_x;
        server->grab_y = server->cursor->y - deco_y;
        server->grab_start_x = server->cursor->x;
        server->grab_start_y = server->cursor->y;
    } else {
        const struct design_config *d = &view->server->design;
        int content_x = view->x + d->border;
        int content_y = view->y + d->title_h + d->border;
        struct wlr_box geo_box = view->xdg_toplevel->base->geometry;

        double border_x = content_x +
            ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
        double border_y = content_y +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;
        server->grab_geobox = geo_box;
        server->grab_geobox.x += content_x;
        server->grab_geobox.y += content_y;
        server->resize_edges = edges;
    }
    wlr_log(WLR_DEBUG, "begin_interactive: view=%p mode=%d edges=0x%x",
            (void *)view, mode, edges);
}

/*
 * GNOME edge tiling: бросил окно у левого/правого края — тайл на половину,
 * у верхнего края — максимизация. Вызывается при завершении MOVE-жеста
 * и только если окно реально перетаскивалось (иначе случайный клик у
 * края тайлил окно без ведома пользователя).
 */
#define TILE_EDGE_PX 12
#define TILE_DRAG_MIN_PX 24

static void move_end_edge_actions(struct mywm_server *server) {
    struct mywm_view *view = server->grabbed_view;
    if (view == NULL || server->cursor_mode != MYWM_CURSOR_MOVE) {
        return;
    }
    double dx = server->cursor->x - server->grab_start_x;
    double dy = server->cursor->y - server->grab_start_y;
    if (sqrt(dx * dx + dy * dy) < TILE_DRAG_MIN_PX) {
        return;
    }
    const struct design_config *d = &server->design;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    struct wlr_box usable = shell_usable_box(server);
    double cx = server->cursor->x;
    double cy = server->cursor->y;
    /* Учитываем точку под курсором И край самого окна. */
    bool left = cx <= box.x + TILE_EDGE_PX ||
        view->x <= box.x + 2;
    bool right = cx >= box.x + box.width - TILE_EDGE_PX ||
        view->x + view->width + 2 * d->border >=
            box.x + box.width - 2;
    bool top = usable.height > 0 &&
        cy <= usable.y + TILE_EDGE_PX;
    if (top && !left && !right) {
        maximize_view(view);
        return;
    }
    if (left) {
        tile_view(view, 1);
    } else if (right) {
        tile_view(view, 2);
    }
}

/*
 * Кнопка мыши: отпускание завершает move/resize (сброс в PASSTHROUGH).
 * Нажатие переключает фокус на view под курсором; при зажатом Super
 * дополнительно стартует перемещение окна.
 */
static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    if (event == NULL) {
        return;
    }
    wlr_seat_pointer_notify_button(server->seat, event->time_msec,
                                   event->button, event->state);
    idle_notify_activity(server);
    /* Сессия заблокирована: события уходят только лок-поверхности. */
    if (session_lock_active(server)) {
        return;
    }
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        move_end_edge_actions(server);
        reset_cursor_mode(server);
        return;
    }
    double cx = server->cursor->x;
    double cy = server->cursor->y;

    /* Открытое меню приложений поглощает клики: запуск иконки или
     * закрытие по клику мимо. */
    if (apps_menu_is_open(server)) {
        apps_menu_click(server, cx, cy);
        return;
    }

    /* Клик по кнопкам максимизированного окна в менюбаре. */
    enum mywm_title_button bar_btn = bar_button_at(server, cx, cy);
    if (bar_btn != MYWM_BTN_NONE) {
        if (server->focused_view != NULL) {
            switch (bar_btn) {
            case MYWM_BTN_CLOSE:
                close_view(server->focused_view);
                break;
            case MYWM_BTN_MINIMIZE:
                minimize_view(server->focused_view);
                break;
            case MYWM_BTN_MAXIMIZE:
                maximize_view(server->focused_view);
                break;
            default:
                break;
            }
        }
        return;
    }

    /* Клик по иконке дока: фокус/восстановление окна либо запуск
     * закреплённого приложения. */
    if (dock_activate_at(server, cx, cy)) {
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct mywm_view *view = desktop_view_at(server, cx, cy, &surface, &sx, &sy);
    if (view == NULL) {
        view = desktop_deco_at(server, cx, cy);
    }
    if (view == NULL) {
        return;
    }
    if (surface != NULL) {
        focus_view(server, view, surface);
    } else {
        focus_view(server, view, view->xdg_toplevel->base->surface);
    }

    enum mywm_title_button btn = title_button_at(view, cx, cy);
    switch (btn) {
    case MYWM_BTN_CLOSE:
        close_view(view);
        return;
    case MYWM_BTN_MINIMIZE:
        minimize_view(view);
        return;
    case MYWM_BTN_MAXIMIZE:
        maximize_view(view);
        return;
    default:
        break;
    }

    bool double_click =
        event->time_msec - server->last_press_time < BTN_DOUBLE_CLICK_MS &&
        fabs(cx - server->last_press_x) < 5 &&
        fabs(cy - server->last_press_y) < 5;
    server->last_press_time = event->time_msec;
    server->last_press_x = cx;
    server->last_press_y = cy;

    if (point_in_title(view, cx, cy)) {
        if (double_click) {
            maximize_view(view);
            return;
        }
        if (!view->maximized) {
            begin_interactive(view, MYWM_CURSOR_MOVE, 0);
        }
        return;
    }
    if (server->mod_pressed) {
        begin_interactive(view, MYWM_CURSOR_MOVE, 0);
        return;
    }
    if (surface == NULL) {
        uint32_t edges = view_resize_edges(view, cx, cy);
        if (edges != 0) {
            begin_interactive(view, MYWM_CURSOR_RESIZE, edges);
        }
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    if (event == NULL) {
        return;
    }
    idle_notify_activity(server);
    /* Колесо при открытом меню приложений листает сетку. */
    if (apps_menu_is_open(server)) {
        apps_menu_scroll(server, event->delta, event->source);
        wlr_seat_pointer_notify_frame(server->seat);
        return;
    }
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
                                 event->orientation, event->delta,
                                 event->delta_discrete, event->source,
                                 event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, cursor_frame);
    (void)data;
    wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client =
        server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server->cursor, event->surface,
                               event->hotspot_x, event->hotspot_y);
    }
}

void server_new_pointer(struct mywm_server *server,
                        struct wlr_input_device *device) {
    if (server == NULL || device == NULL) {
        return;
    }
    wlr_cursor_attach_input_device(server->cursor, device);
}

void cursor_init(struct mywm_server *server) {
    server->cursor = wlr_cursor_create();
    if (server->cursor == NULL) {
        wlr_log(WLR_ERROR, "Failed to create wlr_cursor");
        return;
    }
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);
    server->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(server->cursor_mgr, 1);

    server->cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
    server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
    server->cursor_button.notify = server_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);
    server->cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
    server->cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

    server->request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
}