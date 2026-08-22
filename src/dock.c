#include "server.h"
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* macOS-style Dock Configuration.
 * Размер иконки/отступы/зазор — в server->design ([design] config.toml). */
#define DOCK_MAG_EXTRA 24          /* Увеличенное увеличение как в macOS */
#define DOCK_MAG_SIGMA 35.0        /* Более широкое распространение волны */
#define DOCK_SEP_WIDTH 1           /* Тонкий разделитель */
#define DOCK_DOT_W 6
#define DOCK_DOT_H 6
#define DOCK_DOT_RADIUS 3
#define DOCK_ANIM_MS 16            /* Плавная анимация ~60fps */
#define DOCK_ANIM_SPEED 0.25       /* Более плавное затухание */

static int dock_height(const struct mywm_server *server) {
    return server->design.dock_icon + 2 * server->design.dock_pad + 8;
}
static int dock_dot_y(const struct mywm_server *server) {
    return dock_height(server) - server->design.dock_pad + 4;
}

static void dock_layout(struct mywm_server *server);

/*
 * Целевые размеры: иконки растут только когда курсор реально наведён на одну
 * из них; волна распространяется от наведённой иконки к соседям (гаусс).
 */
static void dock_sizes(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    struct mywm_dock_item *it;
    const int icon = server->design.dock_icon;

    struct mywm_view *hv = dock_icon_at(server, server->cursor->x,
                                        server->cursor->y);
    struct mywm_dock_item *hovered = NULL;
    if (hv != NULL) {
        wl_list_for_each(it, &dock->items, link) {
            if (it->view == hv) {
                hovered = it;
                break;
            }
        }
    }
    double hx = 0.0;
    if (hovered != NULL) {
        hx = hovered->lx + hovered->lw / 2.0;
    }
    wl_list_for_each(it, &dock->items, link) {
        if (!it->view->mapped && !it->view->minimized) {
            continue;
        }
        double mag = 0.0;
        if (hovered != NULL) {
            double dx = (it->lx + it->lw / 2.0) - hx;
            mag = DOCK_MAG_EXTRA *
                exp(-(dx * dx) / (2.0 * DOCK_MAG_SIGMA * DOCK_MAG_SIGMA));
        }
        it->tw = icon + mag;
        it->th = icon + mag;
    }
}

/*
 * Таймер анимации: каждый тик текущие размеры стремятся к целевым
 * (экспоненциальное сглаживание), затем раскладка заново. Когда всё
 * сошлось — таймер останавливается.
 */
static int dock_anim_tick(void *data) {
    struct mywm_server *server = data;
    struct mywm_dock *dock = &server->dock;
    bool done = true;
    struct mywm_dock_item *it;
    wl_list_for_each(it, &dock->items, link) {
        it->cw += (it->tw - it->cw) * DOCK_ANIM_SPEED;
        if (fabs(it->tw - it->cw) > 0.5) {
            done = false;
        } else {
            it->cw = it->tw;
        }
        it->ch = it->cw;
    }
    dock_layout(server);
    wl_event_source_timer_update(dock->anim_timer,
                                 done ? 0 : DOCK_ANIM_MS);
    return 0;
}

/*
 * Пересчёт дока: полоса по центру низа layout, слева — окна, после
 * разделителя — свёрнутые. Иконки растут вверх от нижней линии.
 */
static void dock_layout(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    if (wlr_box_empty(&box)) {
        return;
    }

    int napps = 0, nmin = 0;
    struct mywm_dock_item *it;
    wl_list_for_each(it, &dock->items, link) {
        if (!it->view->mapped && !it->view->minimized) {
            continue;
        }
        if (it->view->minimized) {
            nmin++;
        } else {
            napps++;
        }
    }
    int n = napps + nmin;
    wlr_scene_node_set_enabled(&dock->tree->node, n > 0);
    if (n == 0) {
        return;
    }

    /* Проход 1: размеры (только при наведении на иконку). */
    dock_sizes(server);
    wl_list_for_each(it, &dock->items, link) {
        if (!it->view->mapped && !it->view->minimized) {
            continue;
        }
        it->lw = (int)(it->cw + 0.5);
        it->lh = (int)(it->ch + 0.5);
    }

    /* Проход 2: разметка (окна | разделитель | свёрнутые), по центру. */
    const struct design_config *d = &server->design;
    int total = 0;
    wl_list_for_each(it, &dock->items, link) {
        if (!it->view->mapped && !it->view->minimized) {
            continue;
        }
        total += it->lw + d->dock_gap;
    }
    total -= d->dock_gap;
    bool has_sep = napps > 0 && nmin > 0;
    if (has_sep) {
        total += DOCK_SEP_WIDTH + 2 * d->dock_gap;
    }

    int bh = dock_height(server);
    int bar_x = box.x + (box.width - total - 2 * d->dock_pad) / 2;
    int bar_y = box.y + box.height - bh;
    wlr_scene_node_set_position(&dock->tree->node, bar_x, bar_y);
    wlr_scene_rect_set_size(dock->bar, total + 2 * d->dock_pad, bh);

    wlr_scene_node_set_enabled(&dock->sep->node, has_sep);
    int x = d->dock_pad;
    bool passed_sep = (napps == 0);
    wl_list_for_each(it, &dock->items, link) {
        struct mywm_view *v = it->view;
        if (!v->mapped && !v->minimized) {
            continue;
        }
        bool is_min = v->minimized;
        if (!passed_sep && is_min) {
            wlr_scene_rect_set_size(dock->sep, DOCK_SEP_WIDTH,
                                    d->dock_icon);
            wlr_scene_node_set_position(&dock->sep->node, x, d->dock_pad);
            x += DOCK_SEP_WIDTH + d->dock_gap;
            passed_sep = true;
        }
        it->lx = bar_x + x;
        it->ly = bar_y + d->dock_pad + d->dock_icon - it->lh;

        /* Показываем иконку приложения вместо прямоугольника */
        if (it->icon_img) {
            wlr_scene_node_set_enabled(&it->icon_img->node, true);
            wlr_scene_buffer_set_dest_size(it->icon_img, it->lw, it->lh);
            wlr_scene_node_set_position(&it->icon_img->node, x,
                                        d->dock_pad + d->dock_icon - it->lh);
            /* Прячем fallback прямоугольник */
            wlr_scene_node_set_enabled(&it->icon->node, false);
        } else {
            /* Если нет текстуры иконки, показываем fallback */
            wlr_scene_rect_set_size(it->icon, it->lw, it->lh);
            wlr_scene_node_set_position(&it->icon->node, x,
                                        d->dock_pad + d->dock_icon - it->lh);
            const float *color = d->dock_idle;
            if (v == server->focused_view) {
                color = d->dock_active;
            } else if (is_min) {
                color = d->dock_minimized;
            }
            wlr_scene_rect_set_color(it->icon, color);
            wlr_scene_node_set_enabled(&it->icon->node, true);
        }
        x += it->lw + d->dock_gap;
    }

    /* Точка под иконкой активного приложения. */
    struct mywm_view *focused = server->focused_view;
    struct mywm_dock_item *active = NULL;
    wl_list_for_each(it, &dock->items, link) {
        if (it->view == focused) {
            active = it;
            break;
        }
    }
    bool show_dot = active != NULL &&
        (active->view->mapped || active->view->minimized);
    wlr_scene_node_set_enabled(&dock->dot->node, show_dot);
    if (show_dot) {
        wlr_scene_rect_set_size(dock->dot, DOCK_DOT_W, DOCK_DOT_H);
        wlr_scene_node_set_position(&dock->dot->node,
                                    active->lx - bar_x +
                                        (active->lw - DOCK_DOT_W) / 2,
                                    dock_dot_y(server));
    }
}

void dock_update(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    dock_sizes(server);
    bool need_anim = false;
    struct mywm_dock_item *it;
    wl_list_for_each(it, &dock->items, link) {
        if (fabs(it->tw - it->cw) > 0.1) {
            need_anim = true;
            break;
        }
    }
    if (need_anim) {
        wl_event_source_timer_update(dock->anim_timer, 1);
    }
}

void dock_refresh(struct mywm_server *server) {
    dock_layout(server);
}

void dock_init(struct mywm_server *server) {
    server->dock.server = server;
    const struct design_config *d = &server->design;
    server->dock.tree = wlr_scene_tree_create(&server->scene->tree);
    server->dock.bar = wlr_scene_rect_create(server->dock.tree, 0, 0,
                                             d->dock_bg);
    server->dock.sep = wlr_scene_rect_create(server->dock.tree,
                                             DOCK_SEP_WIDTH, d->dock_icon,
                                             d->dock_sep);
    wlr_scene_node_set_enabled(&server->dock.sep->node, false);
    server->dock.dot = wlr_scene_rect_create(server->dock.tree,
                                             DOCK_DOT_W, DOCK_DOT_H,
                                             d->dock_dot);
    wlr_scene_node_set_enabled(&server->dock.dot->node, false);
    wl_list_init(&server->dock.items);
    struct wl_event_loop *loop =
        wl_display_get_event_loop(server->wl_display);
    server->dock.anim_timer =
        wl_event_loop_add_timer(loop, dock_anim_tick, server);
    dock_refresh(server);
}

void dock_add_view(struct mywm_server *server, struct mywm_view *view) {
    struct mywm_dock_item *item = calloc(1, sizeof(struct mywm_dock_item));
    if (item == NULL) {
        return;
    }
    item->view = view;
    const struct design_config *d = &server->design;

    /* Создаём fallback прямоугольник (будет скрыт если есть иконка) */
    item->icon = wlr_scene_rect_create(server->dock.tree, d->dock_icon,
                                       d->dock_icon, d->dock_idle);
    wlr_scene_node_set_enabled(&item->icon->node, false);

    /* Пытаемся загрузить иконку приложения по app_id */
    const char *app_id = NULL;
    if (view->xdg_toplevel && view->xdg_toplevel->base) {
        app_id = view->xdg_toplevel->app_id;
    }

    if (app_id != NULL && strlen(app_id) > 0) {
        item->icon_img = icon_load_app(&server->icon_mgr, server->dock.tree,
                                        app_id, d->dock_icon);
    }
    
    /* Если иконка не загрузилась, создаём цветной fallback */
    if (item->icon_img == NULL) {
        static const float fallback_colors[][4] = {
            {0.2f, 0.6f, 1.0f, 0.9f},
            {1.0f, 0.4f, 0.2f, 0.9f},
            {0.2f, 0.8f, 0.4f, 0.9f},
            {0.8f, 0.2f, 0.6f, 0.9f},
        };
        unsigned hash = 0;
        if (app_id) {
            for (const char *p = app_id; *p; p++) {
                hash = hash * 31 + (unsigned)*p;
            }
        }
        item->icon_img = icon_create_fallback(&server->icon_mgr,
                                              server->dock.tree,
                                              d->dock_icon,
                                              fallback_colors[hash % 4]);
    }

    /* Скрываем иконку по умолчанию, показываем только в layout */
    if (item->icon_img) {
        wlr_scene_node_set_enabled(&item->icon_img->node, false);
        wlr_scene_buffer_set_dest_size(item->icon_img, d->dock_icon,
                                       d->dock_icon);
    }

    item->cw = item->ch = item->tw = item->th = d->dock_icon;
    wl_list_insert(&server->dock.items, &item->link);
    dock_refresh(server);
}

void dock_remove_view(struct mywm_server *server, struct mywm_view *view) {
    struct mywm_dock_item *it, *tmp;
    wl_list_for_each_safe(it, tmp, &server->dock.items, link) {
        if (it->view != view) {
            continue;
        }
        wlr_scene_node_destroy(&it->icon->node);
        if (it->icon_img) {
            wlr_scene_node_destroy(&it->icon_img->node);
        }
        wl_list_remove(&it->link);
        free(it);
        break;
    }
    dock_refresh(server);
}

struct mywm_view *dock_icon_at(struct mywm_server *server,
                               double lx, double ly) {
    struct mywm_dock *dock = &server->dock;
    if (dock->tree == NULL || !dock->tree->node.enabled) {
        return NULL;
    }
    struct mywm_dock_item *it;
    wl_list_for_each(it, &dock->items, link) {
        struct mywm_view *v = it->view;
        if (!v->mapped && !v->minimized) {
            continue;
        }
        if (lx >= it->lx && lx < it->lx + it->lw &&
                ly >= it->ly && ly < it->ly + it->lh) {
            return v;
        }
    }
    return NULL;
}

void dock_raise(struct mywm_server *server) {
    wlr_scene_node_raise_to_top(&server->dock.tree->node);
}
/* Повторное применение [design] к доку (SIGHUP): цвета панели,
 * разделителя и точки; размеры применит dock_layout. */
void dock_redesign(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    if (dock->tree == NULL) {
        return;
    }
    const struct design_config *d = &server->design;
    wlr_scene_rect_set_color(dock->bar, d->dock_bg);
    wlr_scene_rect_set_color(dock->sep, d->dock_sep);
    wlr_scene_rect_set_color(dock->dot, d->dock_dot);
    dock_refresh(server);
}
