#include "server.h"
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* macOS-style Dock Configuration */
#define DOCK_ICON 48
#define DOCK_MAG_EXTRA 24          /* Увеличенное увеличение как в macOS */
#define DOCK_MAG_SIGMA 35.0        /* Более широкое распространение волны */
#define DOCK_GAP 6                 /* Меньший зазор между иконками */
#define DOCK_PADDING 8             /* Меньшие отступы */
#define DOCK_HEIGHT (DOCK_ICON + 2 * DOCK_PADDING + 8)
#define DOCK_SEP_WIDTH 1           /* Тонкий разделитель */
#define DOCK_DOT_W 6
#define DOCK_DOT_H 6
#define DOCK_DOT_RADIUS 3
#define DOCK_DOT_Y (DOCK_HEIGHT - DOCK_PADDING + 4)
#define DOCK_ANIM_MS 16            /* Плавная анимация ~60fps */
#define DOCK_ANIM_SPEED 0.25       /* Более плавное затухание */
#define DOCK_CORNER_RADIUS 12      /* Закругление панели дока */

/* macOS-inspired color palette (Space Gray theme) */
static const float dock_bar_color[4] = {0.12f, 0.12f, 0.14f, 0.75f};  /* Полупрозрачный тёмный */
static const float dock_icon_color[4] = {0.50f, 0.52f, 0.56f, 0.95f};  /* Светло-серый неактивный */
static const float dock_icon_active[4] = {1.00f, 1.00f, 1.00f, 1.00f};  /* Белый активный */
static const float dock_icon_minimized[4] = {0.35f, 0.36f, 0.40f, 0.85f}; /* Приглушённый для свёрнутых */
static const float dock_sep_color[4] = {0.50f, 0.50f, 0.52f, 0.35f};   /* Еле заметный разделитель */
static const float dock_dot_color[4] = {1.00f, 1.00f, 1.00f, 0.90f};   /* Белый индикатор */
static const float dock_glow_color[4] = {0.40f, 0.60f, 1.00f, 0.15f};  /* Голубое свечение */

static void dock_layout(struct mywm_server *server);

/*
 * Целевые размеры: иконки растут только когда курсор реально наведён на одну
 * из них; волна распространяется от наведённой иконки к соседям (гаусс).
 */
static void dock_sizes(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    struct mywm_dock_item *it;

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
        it->tw = DOCK_ICON + mag;
        it->th = DOCK_ICON + mag;
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
    int total = 0;
    wl_list_for_each(it, &dock->items, link) {
        if (!it->view->mapped && !it->view->minimized) {
            continue;
        }
        total += it->lw + DOCK_GAP;
    }
    total -= DOCK_GAP;
    bool has_sep = napps > 0 && nmin > 0;
    if (has_sep) {
        total += DOCK_SEP_WIDTH + 2 * DOCK_GAP;
    }

    int bar_x = box.x + (box.width - total - 2 * DOCK_PADDING) / 2;
    int bar_y = box.y + box.height - DOCK_HEIGHT;
    wlr_scene_node_set_position(&dock->tree->node, bar_x, bar_y);
    wlr_scene_rect_set_size(dock->bar, total + 2 * DOCK_PADDING,
                            DOCK_HEIGHT);

    wlr_scene_node_set_enabled(&dock->sep->node, has_sep);
    int x = DOCK_PADDING;
    bool passed_sep = (napps == 0);
    wl_list_for_each(it, &dock->items, link) {
        struct mywm_view *v = it->view;
        if (!v->mapped && !v->minimized) {
            continue;
        }
        bool is_min = v->minimized;
        if (!passed_sep && is_min) {
            wlr_scene_rect_set_size(dock->sep, DOCK_SEP_WIDTH, DOCK_ICON);
            wlr_scene_node_set_position(&dock->sep->node, x, DOCK_PADDING);
            x += DOCK_SEP_WIDTH + DOCK_GAP;
            passed_sep = true;
        }
        it->lx = bar_x + x;
        it->ly = bar_y + DOCK_PADDING + DOCK_ICON - it->lh;
        wlr_scene_rect_set_size(it->icon, it->lw, it->lh);
        wlr_scene_node_set_position(&it->icon->node, x,
                                    DOCK_PADDING + DOCK_ICON - it->lh);
        const float *color = dock_icon_color;
        if (v == server->focused_view) {
            color = dock_icon_active;
        } else if (is_min) {
            color = dock_icon_minimized;
        }
        wlr_scene_rect_set_color(it->icon, color);
        x += it->lw + DOCK_GAP;
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
                                    DOCK_DOT_Y);
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
    server->dock.tree = wlr_scene_tree_create(&server->scene->tree);
    server->dock.bar = wlr_scene_rect_create(server->dock.tree, 0, 0,
                                             dock_bar_color);
    server->dock.sep = wlr_scene_rect_create(server->dock.tree,
                                             DOCK_SEP_WIDTH, DOCK_ICON,
                                             dock_sep_color);
    wlr_scene_node_set_enabled(&server->dock.sep->node, false);
    server->dock.dot = wlr_scene_rect_create(server->dock.tree,
                                             DOCK_DOT_W, DOCK_DOT_H,
                                             dock_dot_color);
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
    item->icon = wlr_scene_rect_create(server->dock.tree, DOCK_ICON,
                                       DOCK_ICON, dock_icon_color);
    item->cw = item->ch = item->tw = item->th = DOCK_ICON;
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