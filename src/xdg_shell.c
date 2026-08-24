#include "server.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

static void view_title_update(struct mywm_view *view);
static void view_buttons_apply(struct mywm_view *view);

/*
 * Размеры хрома привязаны к размеру содержимого окна (view->width/height),
 * который приходит от клиента в commit. Текстура пересоздаётся только при
 * смене размера/цвета; во время трансформации размеры ведёт анимация.
 */
static void update_decorations(struct mywm_view *view) {
    if (view->tform_active) {
        return;
    }
    const struct design_config *d = &view->server->design;
    int cw = view->width + 2 * d->border;
    int ch = view->height + d->title_h + 2 * d->border;
    if (view->chrome_buf == NULL || cw != view->chrome_w || ch != view->chrome_h) {
        effects_chrome_regen(view);
    } else {
        wlr_scene_buffer_set_dest_size(view->chrome, cw, ch);
        wlr_scene_node_set_position(&view->chrome->node, 0, 0);
        effects_shadow_reset_alpha(view, 1.0f);
    }
    view_title_update(view);
}

/* --- Заголовок окна в тайтлбаре (Adwaita): полужирный, по центру. --- */
#define TITLE_FONT_PX 12

/* Естественная позиция заголовка (центр тайтлбара). */
static void view_title_natural_pos(const struct mywm_view *view,
                                   int *x, int *y) {
    const struct design_config *d = &view->server->design;
    int tw = view->title_buf != NULL ? view->title_buf->base.width : 0;
    int th = view->title_buf != NULL ? view->title_buf->base.height : 0;
    *x = d->border + (view->width - tw) / 2;
    *y = d->border + (d->title_h - th) / 2;
    if (*y < 0) {
        *y = 0;
    }
}

void view_title_apply_anim(struct mywm_view *view, double bs,
                           double off_x, double off_y, float alpha) {
    if (view == NULL || view->title_node == NULL ||
            view->title_buf == NULL) {
        return;
    }
    int tw = view->title_buf->base.width;
    int th = view->title_buf->base.height;
    int dw = (int)(tw * bs + 0.5);
    int dh = (int)(th * bs + 0.5);
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    int nx, ny;
    view_title_natural_pos(view, &nx, &ny);
    wlr_scene_buffer_set_dest_size(view->title_node, dw, dh);
    wlr_scene_node_set_position(&view->title_node->node,
                                off_x + nx * bs, off_y + ny * bs);
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    wlr_scene_buffer_set_opacity(view->title_node, alpha);
    wlr_scene_node_set_enabled(&view->title_node->node, true);
}

void view_title_reset_anim(struct mywm_view *view, float alpha) {
    if (view == NULL || view->title_node == NULL ||
            view->title_buf == NULL) {
        return;
    }
    int nx, ny;
    view_title_natural_pos(view, &nx, &ny);
    wlr_scene_buffer_set_dest_size(view->title_node, 0, 0);
    wlr_scene_node_set_position(&view->title_node->node, nx, ny);
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    wlr_scene_buffer_set_opacity(view->title_node, alpha);
    wlr_scene_node_set_enabled(&view->title_node->node, true);
}

static void view_title_update(struct mywm_view *view) {
    if (view->title_node == NULL || view->xdg_toplevel == NULL) {
        return;
    }
    const char *title = view->xdg_toplevel->title;
    if (title == NULL || title[0] == '\0') {
        wlr_scene_node_set_enabled(&view->title_node->node, false);
        return;
    }
    /* Перестраиваем буфер только при смене текста. */
    if (view->title_cache == NULL ||
            strcmp(view->title_cache, title) != 0) {
        free(view->title_cache);
        view->title_cache = strdup(title);
        if (view->title_buf != NULL) {
            wlr_buffer_unlock(&view->title_buf->base);
            view->title_buf = NULL;
        }
        /* Adwaita: полужирный заголовок по центру хедера. */
        view->title_buf = shell_label_buf_weight(view->server, title,
                                                 TITLE_FONT_PX,
                                                 CAIRO_FONT_WEIGHT_BOLD);
        if (view->title_buf != NULL) {
            wlr_buffer_lock(&view->title_buf->base);
            wlr_scene_buffer_set_buffer(view->title_node,
                                        &view->title_buf->base);
        }
    }
    if (view->title_buf == NULL) {
        wlr_scene_node_set_enabled(&view->title_node->node, false);
        return;
    }
    wlr_scene_node_set_enabled(&view->title_node->node, true);

    /* Геометрия — через общий reset (учитывает mid-zoom коммиты). */
    if (view->anim_active &&
            fabs(view->spr_scale.current - 1.0) > 0.001) {
        /* Окно в процессе zoom-анимации: геометрию ведёт тик,
         * чтобы заголовок не мигал между натуральным и масштабом. */
        view_title_apply_anim(view, view->spr_scale.current, 0, 0, 1.0f);
    } else {
        view_title_reset_anim(view, 1.0f);
    }
}

/* --- Кнопки управления окном (traffic lights, слева как в macOS). --- */

static void view_buttons_apply(struct mywm_view *view) {
    struct design_config *d = &view->server->design;
    const float *cols[3] = {d->btn_close, d->btn_minimize, d->btn_maximize};
    for (int i = 0; i < 3; i++) {
        mywm_btn_recreate(view->deco_tree, &view->btns[i], d->btn_size,
                          cols[i], (enum mywm_title_button)(i + 1));
        wlr_scene_node_set_position(
            &view->btns[i].node->node,
            d->border + BTN_X + i * (d->btn_size + d->btn_gap),
            (d->title_h - d->btn_size) / 2);
    }
}

/*
 * Фокус переключается на view. Поднимает scene-ноду наверх по Z-порядку
 * (поэтому view рендерится поверх остальных), деактивирует предыдущую
 * сфокусированную view и оповещает клавиатуру и указатель через seat.
 *
 * Список server->views здесь НЕ перестраивается: он хранит порядок создания.
 * Визуальный Z-порядок обеспечивается wlr_scene_node_raise_to_top.
 */
void focus_view(struct mywm_server *server, struct mywm_view *view,
                struct wlr_surface *surface) {
    if (server == NULL || view == NULL || surface == NULL || !view->mapped) {
        return;
    }
    struct wlr_seat *seat = server->seat;
    if (seat == NULL) {
        return;
    }
    /* Сворачивание отключает узел декораций; клик по иконке дока (или
     * Cmd+Tab) запускает обратную genie-анимацию, фокус придёт после
     * её завершения (effects_tform_finalize). */
    if (view->minimized) {
        if (!view->tform_active) {
            effects_tform_start(view, TFORM_GENIE_OUT);
        }
        return;
    }
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface != NULL) {
        struct wlr_xdg_toplevel *prev_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
            struct wlr_scene_tree *prev_tree = prev_toplevel->base->data;
            struct mywm_view *prev_view = prev_tree->node.data;
            if (prev_view != NULL) {
                effects_chrome_regen(prev_view);
                view_buttons_apply(prev_view);
            }
        }
    }

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&view->deco_tree->node);
    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
    server->focused_view = view;
    effects_chrome_regen(view);
    view_buttons_apply(view);
    dock_refresh(server);
    dock_raise(server);
    bar_raise(server);
    bar_update_name(server);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(seat, surface,
                                       keyboard->keycodes, keyboard->num_keycodes,
                                       &keyboard->modifiers);
    }
    /* Пересчитываем точку входа указателя относительно поверхности.
     * Для toplevel-поверхности без смещения геометрии достаточно вычесть
     * позицию scene-ноды. */
    double sx = server->cursor->x - view->scene_tree->node.x;
    double sy = server->cursor->y - view->scene_tree->node.y;
    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);

    wlr_log(WLR_DEBUG, "focus_view: view=%p surface=%p", (void *)view, (void *)surface);
}

void close_view(struct mywm_view *view) {
    if (view != NULL && view->mapped) {
        wlr_xdg_toplevel_send_close(view->xdg_toplevel);
        wlr_log(WLR_DEBUG, "close_view: view=%p", (void *)view);
    }
}

void minimize_view(struct mywm_view *view) {
    if (view == NULL || view->minimized || view->closing ||
            view->tform_active) {
        return;
    }
    struct mywm_server *server = view->server;
    if (server->focused_view == view) {
        server->focused_view = NULL;
        wlr_seat_keyboard_clear_focus(server->seat);
    }
    if (server->grabbed_view == view) {
        reset_cursor_mode(server);
    }
    effects_tform_start(view, TFORM_GENIE_IN);
    bar_update_name(server);
    wlr_log(WLR_DEBUG, "minimize_view: view=%p", (void *)view);
}

/*
 * Развернуть/свернуть максимизацию (как зелёная кнопка macOS): клиенту
 * сразу отправляется configure на целевой размер (он подтвердит новым
 * буфером в commit), а композитор анимирует масштаб/позицию хрома через
 * трансформацию. Точная геометрия — в effects_tform_finalize.
 */
void maximize_view(struct mywm_view *view) {
    if (view == NULL || !view->mapped || view->closing ||
            view->tform_active) {
        return;
    }
    struct mywm_server *server = view->server;
    if (server->grabbed_view == view) {
        reset_cursor_mode(server);
    }
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    if (view->maximized) {
        view->maximized = false;
        wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                  view->save_w, view->save_h);
        effects_tform_start(view, TFORM_UNMAXIMIZE);
    } else {
        view->maximized = true;
        view->save_x = view->x;
        view->save_y = view->y;
        view->save_w = view->width;
        view->save_h = view->height;
        const struct design_config *d = &server->design;
        wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                  box.width - 2 * d->border,
                                  box.height - d->menu_bar_h -
                                      d->title_h - d->border);
        effects_tform_start(view, TFORM_MAXIMIZE);
    }
    bar_update_name(server);
    wlr_log(WLR_DEBUG, "maximize_view: view=%p maximized=%d",
            (void *)view, view->maximized);
}

/*
 * GNOME-тайлинг: side 1 — левая половина layout, 2 — правая, 0 —
 * восстановить исходную геометрию. Геометрия до первого тайла
 * запоминается в save_*; повторный тайл на ту же сторону разворачивает.
 */
void tile_view(struct mywm_view *view, int side) {
    if (view == NULL || !view->mapped || view->closing ||
            view->tform_active) {
        return;
    }
    struct mywm_server *server = view->server;
    const struct design_config *d = &server->design;
    if (server->grabbed_view == view) {
        reset_cursor_mode(server);
    }
    if (side == 0 && view->tiled_side == 0 && !view->maximized) {
        /* Восстанавливать нечего. */
        return;
    }
    if (view->maximized) {
        /* Из максимизации сразу в тайл: текущая геометрия не сохраняем. */
        view->maximized = false;
        if (side == 0) {
            wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                      view->save_w, view->save_h);
            effects_view_set_position(view, view->save_x, view->save_y);
            bar_update_name(server);
            return;
        }
    } else if (view->tiled_side == 0 && !view->maximized && side != 0) {
        view->save_x = view->x;
        view->save_y = view->y;
        view->save_w = view->width;
        view->save_h = view->height;
    }

    view->tiled_side = side;
    if (side == 0) {
        wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                  view->save_w, view->save_h);
        effects_view_set_position(view, view->save_x, view->save_y);
    } else {
        struct wlr_box box;
        wlr_output_layout_get_box(server->output_layout, NULL, &box);
        int half_w = box.width / 2 - 2 * d->border;
        int h = box.height - d->menu_bar_h - d->title_h - d->border;
        int x = side == 1 ? box.x : box.x + box.width / 2;
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, half_w, h);
        effects_view_set_position(view, x, box.y + d->menu_bar_h);
        if (server->focused_view == view) {
            effects_chrome_regen(view);
        }
    }
    bar_update_name(server);
    wlr_log(WLR_DEBUG, "tile_view: view=%p side=%d", (void *)view, side);
}

/*
 * map: view стала видимой. Никаких операций со списком — link уже в списке
 * с момента создания (new_toplevel). Позиция восстанавливается из view->x/y.
 */
static void xdg_toplevel_map_handler(struct wl_listener *listener, void *data) {
    struct mywm_view *view = wl_container_of(listener, view, map);
    (void)data;
    view->mapped = true;
    wlr_scene_node_set_position(&view->deco_tree->node, view->x, view->y);
    view_effects_open(view);
    focus_view(view->server, view, view->xdg_toplevel->base->surface);
    dock_refresh(view->server);
    wlr_log(WLR_DEBUG, "toplevel mapped: view=%p pos=(%d,%d)",
            (void *)view, view->x, view->y);
}

/*
 * unmap: view скрыта. Список не трогаем — link остаётся в server->views,
 * фильтрация при рендере/переборе идёт по view->mapped.
 */
static void xdg_toplevel_unmap_handler(struct wl_listener *listener, void *data) {
    struct mywm_view *view = wl_container_of(listener, view, unmap);
    (void)data;
    view->mapped = false;
    if (view->server->grabbed_view == view) {
        reset_cursor_mode(view->server);
    }
    if (view->server->focused_view == view) {
        view->server->focused_view = NULL;
    }
    dock_refresh(view->server);
    bar_update_name(view->server);
    wlr_log(WLR_DEBUG, "toplevel unmapped: view=%p", (void *)view);
}

/*
 * commit: клиент подтвердил новый буфер. Здесь синхронизируем актуальную
 * геометрию (размер, подтверждённый клиентом) и позицию scene-ноды.
 */
/* Первый buffer-узел с содержимым в поддереве: поверхность лежит под
 * subsurface-tree, созданным wlr_scene_xdg_surface_create, поэтому поиск
 * рекурсивный. */
static struct wlr_scene_buffer *find_first_buffer(struct wlr_scene_tree *tree) {
    struct wlr_scene_node *node;
    wl_list_for_each(node, &tree->children, link) {
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
            if (sb->buffer != NULL) {
                return sb;
            }
        } else if (node->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_buffer *sb =
                find_first_buffer(wlr_scene_tree_from_node(node));
            if (sb != NULL) {
                return sb;
            }
        }
    }
    return NULL;
}

static void xdg_toplevel_commit_handler(struct wl_listener *listener, void *data) {
    struct mywm_view *view = wl_container_of(listener, view, commit);
    (void)data;
    struct wlr_xdg_surface *base = view->xdg_toplevel->base;
    if (base->initial_commit) {
        wlr_xdg_surface_schedule_configure(base);
    }
    /* До map view->x/y — начальная позиция, заданная композитором;
     * синхронизировать их с scene-нодой можно только после map. */
    if (view->mapped) {
        view->x = view->deco_tree->node.x;
        view->y = view->deco_tree->node.y;
    }
    view->width = base->geometry.width;
    view->height = base->geometry.height;
    update_decorations(view);
    /* app_id/title приходят после создания: разово привязываем док. */
    if (!view->dock_resolved) {
        const char *aid = view->xdg_toplevel->app_id;
        if (aid != NULL && aid[0] != '\0') {
            view->dock_resolved = true;
            dock_resolve_view(view->server, view, aid);
        }
    }
    /* Содержимое клиента: первый buffer-ребёнок scene_tree. Появляется
     * только после первого commit клиента. */
    if (view->content_buffer == NULL) {
        view->content_buffer = find_first_buffer(view->scene_tree);
    }
}

/*
 * scene_destroy: wlroots уничтожил scene_tree (поверхность клиента умерла).
 * Обнуляем указатели, чтобы тик анимации и view_destroy_final не трогали
 * освобождённую память (в т.ч. content_buffer).
 */
static void view_scene_destroy_handler(struct wl_listener *listener, void *data) {
    struct mywm_view *view = wl_container_of(listener, view, scene_destroy);
    (void)data;
    wl_list_remove(&view->scene_destroy.link);
    view->scene_tree = NULL;
    view->content_buffer = NULL;
}

/*
 * destroy: view уничтожена. Запускает анимацию закрытия (view_effects_close):
 * если анимации включены, реальное освобождение (view_destroy_final)
 * выполнится по завершении тика; иначе — немедленно здесь.
 * ЕДИНСТВЕННОЕ место, где view->link удаляется из server->views.
 */
static void xdg_toplevel_destroy_handler(struct wl_listener *listener, void *data) {
    struct mywm_view *view = wl_container_of(listener, view, destroy);
    (void)data;
    /* Все listeners снимаются СЕЙЧАС, а не в view_destroy_final: wlroots
     * требует, чтобы к концу эмиссии сигнала destroy списки слушателей
     * toplevel были пусты (assert в wlr_xdg_toplevel.c). */
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    view_effects_close(view);
}

/*
 * Финальное освобождение view: снятие listeners, удаление scene-нод,
 * исключение из списков и free. Вызывается из destroy (анимации выключены)
 * или из тика анимации закрытия.
 */
void view_destroy_final(struct mywm_view *view) {
    struct mywm_server *server = view->server;
    wlr_log(WLR_DEBUG, "toplevel destroyed: view=%p", (void *)view);
    if (server->focused_view == view) {
        server->focused_view = NULL;
    }
    if (server->grabbed_view == view) {
        reset_cursor_mode(server);
    }
    if (server->hovered_view == view) {
        server->hovered_view = NULL;
    }
    dock_remove_view(server, view);
    /* Освобождаем CPU-ресурсы декораций; scene-ноды умрут вместе с деревьями. */
    if (view->title_buf != NULL) {
        wlr_buffer_unlock(&view->title_buf->base);
        view->title_buf = NULL;
    }
    free(view->title_cache);
    view->title_cache = NULL;
    /* scene_tree уничтожает wlroots (wlr_scene_xdg_surface) — здесь он
     * может быть уже уничтожен (view_scene_destroy_handler). */
    if (view->scene_tree != NULL) {
        wlr_scene_node_destroy(&view->scene_tree->node);
    }
    wlr_scene_node_destroy(&view->deco_tree->node);
    wl_list_remove(&view->link);
    free(view);
}

/*
 * Клиент запросил перемещение. Проверяем serial по принципу "клиент должен
 * действовать от имени последнего ввода с указателя", затем стартуем жест.
 */
static void xdg_toplevel_request_move_handler(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_move_event *event = data;
    struct mywm_view *view = wl_container_of(listener, view, request_move);
    if (event == NULL) {
        return;
    }
    if (wlr_seat_validate_pointer_grab_serial(view->server->seat,
            view->xdg_toplevel->base->surface, event->serial)) {
        begin_interactive(view, MYWM_CURSOR_MOVE, 0);
    }
}

static void xdg_toplevel_request_resize_handler(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel_resize_event *event = data;
    struct mywm_view *view = wl_container_of(listener, view, request_resize);
    if (event == NULL) {
        return;
    }
    if (wlr_seat_validate_pointer_grab_serial(view->server->seat,
            view->xdg_toplevel->base->surface, event->serial)) {
        begin_interactive(view, MYWM_CURSOR_RESIZE, event->edges);
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct mywm_view *view = calloc(1, sizeof(struct mywm_view));
    if (view == NULL) {
        return;
    }
    view->server = server;
    view->xdg_toplevel = xdg_toplevel;
    /* Каскадное размещение: каждое новое окно — со сдвигом относительно
     * предыдущего, чтобы окна не перекрывались в точности друг с другом. */
    int idx = 0;
    struct mywm_view *v;
    wl_list_for_each(v, &server->views, link) {
        idx++;
    }
    view->x = 100 + idx * 250;
    view->y = 100 + idx * 50;

    const struct design_config *d = &server->design;
    /* Контейнер декораций: бордюр (весь контур), тело и заголовок.
     * Содержимое (scene_tree) — ребёнок контейнера со сдвигом на
     * (border, title). */
    view->deco_tree = wlr_scene_tree_create(&server->scene->tree);
    if (view->deco_tree == NULL) {
        free(view);
        return;
    }
    wlr_scene_node_set_position(&view->deco_tree->node, view->x, view->y);
    view->deco_border = wlr_scene_rect_create(view->deco_tree, 0, 0,
                                              d->window_border);
    view->deco_body = wlr_scene_rect_create(view->deco_tree, 0, 0,
                                            d->window_body);
    wlr_scene_node_set_position(&view->deco_body->node,
                                d->border, d->title_h);
    view->deco_title = wlr_scene_rect_create(view->deco_tree, 0, 0,
                                             d->title_unfocused);
    wlr_scene_node_set_position(&view->deco_title->node, d->border, 0);

    /* Мягкая тень — самая нижняя нода в дереве декораций. */
    view->shadow = wlr_scene_buffer_create(view->deco_tree, NULL);

    /* Хром окна: одна CPU-текстура со скруглёнными углами поверх rect'ов
     * (эффекты.c). Нода создаётся пустой — текстуру зальёт
     * effects_chrome_regen по первому commit клиента. Кнопки и содержимое
     * создаются ниже, т.е. рендерятся поверх хрома. */
    view->chrome = wlr_scene_buffer_create(view->deco_tree, NULL);

    /* Кнопки управления слева в заголовке (красная/жёлтая/зелёная):
     * цветные у сфокусированного окна, серые у остальных (macOS). */
    view_buttons_apply(view);

    /* Заголовок окна по центру тайтлбара. */
    view->title_node = wlr_scene_buffer_create(view->deco_tree, NULL);
    wlr_scene_node_set_enabled(&view->title_node->node, false);

    view->scene_tree = wlr_scene_xdg_surface_create(view->deco_tree,
                                                    xdg_toplevel->base);
    if (view->scene_tree == NULL) {
        wlr_scene_node_destroy(&view->deco_tree->node);
        free(view);
        return;
    }
    wlr_scene_node_set_position(&view->scene_tree->node,
                                d->border, d->title_h);
    view->scene_tree->node.data = view;
    xdg_toplevel->base->data = view->scene_tree;

    /* Единственная вставка view->link в список — при создании view.
     * Удаление — только в destroy. */
    wl_list_insert(&server->views, &view->link);

    view->map.notify = xdg_toplevel_map_handler;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
    view->unmap.notify = xdg_toplevel_unmap_handler;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
    view->destroy.notify = xdg_toplevel_destroy_handler;
    wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
    view->commit.notify = xdg_toplevel_commit_handler;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
    view->request_move.notify = xdg_toplevel_request_move_handler;
    wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
    view->request_resize.notify = xdg_toplevel_request_resize_handler;
    wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
    view->scene_destroy.notify = view_scene_destroy_handler;
    wl_signal_add(&view->scene_tree->node.events.destroy, &view->scene_destroy);

    dock_add_view(server, view);

    wlr_log(WLR_DEBUG, "new xdg toplevel: view=%p", (void *)view);
}

static void xdg_popup_commit_handler(struct wl_listener *listener, void *data) {
    struct mywm_popup *popup = wl_container_of(listener, popup, commit);
    (void)data;
    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy_handler(struct wl_listener *listener, void *data) {
    struct mywm_popup *popup = wl_container_of(listener, popup, destroy);
    (void)data;
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
    wlr_log(WLR_DEBUG, "xdg popup destroyed");
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, new_xdg_popup);
    struct wlr_xdg_popup *xdg_popup = data;

    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (parent == NULL) {
        return;
    }
    struct wlr_scene_tree *parent_tree = parent->data;
    if (parent_tree == NULL) {
        return;
    }
    xdg_popup->base->data =
        wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    struct mywm_popup *popup = calloc(1, sizeof(struct mywm_popup));
    if (popup == NULL) {
        return;
    }
    popup->server = server;
    popup->xdg_popup = xdg_popup;
    popup->commit.notify = xdg_popup_commit_handler;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = xdg_popup_destroy_handler;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
    wlr_log(WLR_DEBUG, "new xdg popup: popup=%p", (void *)popup);
}

void xdg_shell_init(struct mywm_server *server) {
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 7);
    if (server->xdg_shell == NULL) {
        wlr_log(WLR_ERROR, "Failed to create xdg-shell");
        return;
    }
    server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
    server->new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);
}