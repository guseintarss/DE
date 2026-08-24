/*
 * layer_shell.c — поддержка zwlr-layer-shell-v1 (waybar, AGSv2,
 * QuickShell, swaylock и т.д.).
 *
 * Сцена: глобальные деревья слоёв создаются один раз в порядке
 *   [bg обоев] < BACKGROUND < BOTTOM < view_tree < TOP < OVERLAY
 * поэтому окна (view_tree) всегда между bottom- и top-слоями. Деревья
 * встроенной оболочки (менюбар/док/launchpad) создаются позже и потому
 * рендерятся выше OVERLAY.
 *
 * Раскладка: для каждого выхода проходим слои по порядку и вызываем
 * wlr_scene_layer_surface_v1_configure, передавая кумулятивную полезную
 * область — хелпер сам позиционирует ноду и вычитает exclusive_zone.
 * Итоговые отступы запоминаются в server->reserved_*; на их основе
 * shell_usable_box() даёт область для максимизации/тайлинга окон.
 */
#include "server.h"
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

struct mywm_layer_surface {
    struct wl_list link;            /* server->layer_lists[layer] */
    struct mywm_server *server;
    struct wlr_output *output;
    struct wlr_layer_surface_v1 *surface;
    struct wlr_scene_layer_surface_v1 *scene_layer;
    /* Кэш состояния на момент последней раскладки: если commit не
     * изменил ни одно из этих полей, rearrange не нужен. Без этого
     * configure->commit->configure образует петлю, и клиент рисует
     * без остановки (джаддер + расход батареи). */
    uint32_t cached_anchor;
    int32_t cached_exclusive_zone;
    int32_t cached_margin[4];               /* top/right/bottom/left */
    uint32_t cached_exclusive_edge;
    uint32_t cached_desired_w, cached_desired_h;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener new_popup;
};

static int imax(int a, int b) {
    return a > b ? a : b;
}

/* Изменилось ли состояние поверхности, влияющее на раскладку,
 * с момента последнего arrange_layers. */
static bool layer_layout_changed(const struct mywm_layer_surface *ls) {
    const struct wlr_layer_surface_v1_state *st = &ls->surface->current;
    return st->anchor != ls->cached_anchor ||
        st->exclusive_zone != ls->cached_exclusive_zone ||
        st->margin.top != ls->cached_margin[0] ||
        st->margin.right != ls->cached_margin[1] ||
        st->margin.bottom != ls->cached_margin[2] ||
        st->margin.left != ls->cached_margin[3] ||
        st->exclusive_edge != ls->cached_exclusive_edge ||
        st->desired_width != ls->cached_desired_w ||
        st->desired_height != ls->cached_desired_h;
}

static void layer_cache_update(struct mywm_layer_surface *ls) {
    const struct wlr_layer_surface_v1_state *st = &ls->surface->current;
    ls->cached_anchor = st->anchor;
    ls->cached_exclusive_zone = st->exclusive_zone;
    ls->cached_margin[0] = st->margin.top;
    ls->cached_margin[1] = st->margin.right;
    ls->cached_margin[2] = st->margin.bottom;
    ls->cached_margin[3] = st->margin.left;
    ls->cached_exclusive_edge = st->exclusive_edge;
    ls->cached_desired_w = st->desired_width;
    ls->cached_desired_h = st->desired_height;
}

struct wlr_box shell_usable_box(struct mywm_server *server) {
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    if (wlr_box_empty(&box)) {
        return box;
    }
    box.x += server->reserved_left;
    box.y += server->reserved_top;
    box.width -= server->reserved_left + server->reserved_right;
    box.height -= server->reserved_top + server->reserved_bottom;
    if (server->shell_cfg.builtin) {
        const struct design_config *d = &server->design;
        box.y += d->menu_bar_h;
        box.height -= d->menu_bar_h;
    }
    if (box.width < 0) {
        box.width = 0;
    }
    if (box.height < 0) {
        box.height = 0;
    }
    return box;
}

void shell_relayout(struct mywm_server *server) {
    struct wlr_box box = shell_usable_box(server);
    if (wlr_box_empty(&box)) {
        return;
    }
    const struct design_config *d = &server->design;
    struct mywm_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->mapped || view->minimized || view->closing ||
                view->tform_active) {
            continue;
        }
        if (view->maximized) {
            wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                      box.width - 2 * d->border,
                                      box.height - d->title_h - d->border);
            effects_view_set_position(view, box.x, box.y);
        } else if (view->tiled_side != 0) {
            int half_w = box.width / 2 - 2 * d->border;
            int h = box.height - d->title_h - d->border;
            int x = view->tiled_side == 1 ? box.x : box.x + box.width / 2;
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, half_w, h);
            effects_view_set_position(view, x, box.y);
        }
    }
}

/*
 * Пересобрать все layer-поверхности и полезную область. Порядок слоёв:
 * BACKGROUND -> BOTTOM -> TOP -> OVERLAY (эксклюзивные зоны копятся).
 *
 * Важно: конфигурируем ВСЕ поверхности, включая ещё не отображённые —
 * клиент получает первый configure только так (иначе не пришлёт буфер
 * и никогда не отмапится).
 */
void arrange_layers(struct mywm_server *server) {
    int r_left = 0, r_top = 0, r_right = 0, r_bottom = 0;
    struct mywm_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_box full;
        wlr_output_layout_get_box(server->output_layout,
                                  output->wlr_output, &full);
        struct wlr_box usable = full;
        for (int i = 0; i < SHELL_LAYER_COUNT; i++) {
            struct mywm_layer_surface *ls;
            wl_list_for_each(ls, &server->layer_lists[i], link) {
                if (ls->output != output->wlr_output) {
                    continue;
                }
                wlr_scene_layer_surface_v1_configure(ls->scene_layer,
                                                     &full, &usable);
            }
        }
        r_left = imax(r_left, usable.x - full.x);
        r_top = imax(r_top, usable.y - full.y);
        r_right = imax(r_right,
                       (full.x + full.width) - (usable.x + usable.width));
        r_bottom = imax(r_bottom,
                        (full.y + full.height) - (usable.y + usable.height));
    }
    server->reserved_left = r_left;
    server->reserved_top = r_top;
    server->reserved_right = r_right;
    server->reserved_bottom = r_bottom;

    shell_relayout(server);
}

/* Layer-поверхность запросила клавиатуру — отдаём ей фокус seat'а. */
static void layer_keyboard_focus(struct mywm_layer_surface *ls) {
    struct mywm_server *server = ls->server;
    if (ls->surface->current.keyboard_interactive ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE ||
            !ls->surface->surface->mapped) {
        return;
    }
    struct wlr_seat *seat = server->seat;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    server->focused_view = NULL;
    if (kb != NULL) {
        wlr_seat_keyboard_notify_enter(seat, ls->surface->surface,
                                       kb->keycodes, kb->num_keycodes,
                                       &kb->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat, ls->surface->surface,
                                       NULL, 0, NULL);
    }
}

static void layer_map_handler(struct wl_listener *listener, void *data) {
    struct mywm_layer_surface *ls = wl_container_of(listener, ls, map);
    (void)data;
    layer_cache_update(ls);
    arrange_layers(ls->server);
    layer_keyboard_focus(ls);
}

static void layer_unmap_handler(struct wl_listener *listener, void *data) {
    struct mywm_layer_surface *ls = wl_container_of(listener, ls, unmap);
    (void)data;
    /* Если фокус был у этой поверхности — сбрасываем, чтобы клавиатура
     * вернулась окнам при следующем клике/фокусе. */
    if (ls->server->seat->keyboard_state.focused_surface ==
            ls->surface->surface) {
        wlr_seat_keyboard_clear_focus(ls->server->seat);
    }
    arrange_layers(ls->server);
}

static void layer_commit_handler(struct wl_listener *listener, void *data) {
    struct mywm_layer_surface *ls = wl_container_of(listener, ls, commit);
    (void)data;

    /* Пересобираем раскладку ТОЛЬКО когда состояние, влияющее на неё,
     * изменилось. Configure по каждому коммиту создаёт петлю
     * configure→commit→configure: клиент перерисовывается без остановки
     * (лишняя нагрузка на GPU/CPU, джаддер анимаций). Первый commit
     * всегда проходит здесь (кэш нулевой), так клиент получает свой
     * первый configure и сможет отмапиться. */
    if (layer_layout_changed(ls)) {
        layer_cache_update(ls);
        arrange_layers(ls->server);
    }
    layer_keyboard_focus(ls);
}

/* Popup layer-поверхности (меню waybar и т.п.): живёт в дереве слоя,
 * на первом commit требует configure — иначе клиент не отрисуется. */
struct mywm_layer_popup {
    struct wlr_xdg_popup *popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

static void layer_popup_commit_handler(struct wl_listener *listener,
                                       void *data) {
    struct mywm_layer_popup *lp =
        wl_container_of(listener, lp, commit);
    (void)data;
    if (lp->popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(lp->popup->base);
    }
}

static void layer_popup_destroy_handler(struct wl_listener *listener,
                                        void *data) {
    struct mywm_layer_popup *lp =
        wl_container_of(listener, lp, destroy);
    (void)data;
    wl_list_remove(&lp->commit.link);
    wl_list_remove(&lp->destroy.link);
    free(lp);
}

static void layer_new_popup_handler(struct wl_listener *listener, void *data) {
    struct mywm_layer_surface *ls =
        wl_container_of(listener, ls, new_popup);
    struct wlr_xdg_popup *popup = data;

    wlr_scene_xdg_surface_create(ls->scene_layer->tree, popup->base);

    struct mywm_layer_popup *lp = calloc(1, sizeof(*lp));
    if (lp == NULL) {
        return;
    }
    lp->popup = popup;
    lp->commit.notify = layer_popup_commit_handler;
    wl_signal_add(&popup->base->surface->events.commit, &lp->commit);
    lp->destroy.notify = layer_popup_destroy_handler;
    wl_signal_add(&popup->events.destroy, &lp->destroy);
}

static void layer_destroy_handler(struct wl_listener *listener, void *data) {
    struct mywm_layer_surface *ls = wl_container_of(listener, ls, destroy);
    (void)data;
    wl_list_remove(&ls->link);
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->destroy.link);
    wl_list_remove(&ls->new_popup.link);
    free(ls);
}

static void server_new_layer_surface(struct wl_listener *listener,
                                     void *data) {
    struct mywm_server *server =
        wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *layer_surface = data;

    /* Клиент может не привязать выход (get_layer_surface без output) —
     * назначаем первый доступный, иначе протокол требует уничтожить
     * поверхность. */
    struct wlr_output *output = layer_surface->output;
    if (output == NULL) {
        struct mywm_output *o;
        wl_list_for_each(o, &server->outputs, link) {
            output = o->wlr_output;
            break;
        }
    }
    if (output == NULL) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    int layer = (int)layer_surface->current.layer;
    if (layer < 0 || layer >= SHELL_LAYER_COUNT) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    struct mywm_layer_surface *ls = calloc(1, sizeof(*ls));
    if (ls == NULL) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }
    ls->server = server;
    ls->output = output;
    ls->surface = layer_surface;
    layer_surface->output = output;

    ls->scene_layer = wlr_scene_layer_surface_v1_create(
        server->layer_trees[layer], layer_surface);
    if (ls->scene_layer == NULL) {
        free(ls);
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    ls->map.notify = layer_map_handler;
    wl_signal_add(&layer_surface->surface->events.map, &ls->map);
    ls->unmap.notify = layer_unmap_handler;
    wl_signal_add(&layer_surface->surface->events.unmap, &ls->unmap);
    ls->commit.notify = layer_commit_handler;
    wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);
    ls->destroy.notify = layer_destroy_handler;
    wl_signal_add(&layer_surface->events.destroy, &ls->destroy);
    ls->new_popup.notify = layer_new_popup_handler;
    wl_signal_add(&layer_surface->events.new_popup, &ls->new_popup);

    wl_list_insert(&server->layer_lists[layer], &ls->link);
    wlr_log(WLR_DEBUG, "new layer surface: layer=%d output=%s",
            layer, output->name);
}

void layer_shell_init(struct mywm_server *server) {
    for (int i = 0; i < SHELL_LAYER_COUNT; i++) {
        wl_list_init(&server->layer_lists[i]);
    }

    /* Z-порядок снизу вверх: BACKGROUND, BOTTOM, окна (view_tree),
     * TOP, OVERLAY. Всё создаётся здесь, до деревьев dock/bar/apps_menu
     * (они окажутся ещё выше OVERLAY). */
    server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
        wlr_scene_tree_create(&server->scene->tree);
    server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
        wlr_scene_tree_create(&server->scene->tree);
    server->view_tree = wlr_scene_tree_create(&server->scene->tree);
    server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
        wlr_scene_tree_create(&server->scene->tree);
    server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
        wlr_scene_tree_create(&server->scene->tree);

    server->ftl_manager = NULL;

    server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 5);
    if (server->layer_shell == NULL) {
        wlr_log(WLR_ERROR, "Failed to create layer-shell");
        return;
    }
    server->new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server->layer_shell->events.new_surface,
                  &server->new_layer_surface);
}
