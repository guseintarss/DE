#include "server.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* macOS-style Dock Configuration.
 * Размер иконки/отступы/зазор — в server->design ([design] config.toml). */
#define DOCK_MAG_EXTRA 24          /* Увеличенное увеличение как в macOS */
#define DOCK_MAG_SIGMA 55.0        /* Широкая волна: плавный fisheye */
#define DOCK_BAND_MARGIN 14        /* Полоса действия волны вокруг дока, px */
#define DOCK_ANIM_MS 16            /* Плавная анимация ~60fps */
#define DOCK_SPRING_STIFF 170.0    /* Пружина размера иконки (~2 Гц) */
#define DOCK_SPRING_DAMP 22.0      /* Слегка недодемпфирована — мягкий овершут */
#define DOCK_LIFT_MAX 6            /* Доп. подъём растущей иконки, px */
#define DOCK_SEP_WIDTH 1           /* Тонкий разделитель */
#define DOCK_DOT_W 6
#define DOCK_DOT_H 6
#define DOCK_DOT_RADIUS 3
#define DOCK_ANIM_MS 16            /* Плавная анимация ~60fps */
#define DOCK_ANIM_SPEED 0.25       /* Более плавное затухание */

/* Закреплённые приложения: видны всегда, клик запускает команду. */
static const struct {
    const char *app_id;
    const char *command;
} pinned_apps[] = {
    { "org.gnome.Nautilus", "nautilus" },
    { "Alacritty",          "alacritty" },
    { "firefox",            "firefox" },
    { "applications-system", "@apps" },
};

static int dock_height(const struct mywm_server *server) {
    return server->design.dock_icon + 2 * server->design.dock_pad + 8;
}
static int dock_dot_y(const struct mywm_server *server) {
    return dock_height(server) - server->design.dock_pad + 4;
}

static void dock_layout(struct mywm_server *server);

/* Иконка показывается для закреплённых лаунчеров всегда, для окон — когда
 * окно смаплено или свёрнуто (view закреплённого пункта может быть NULL). */
static bool item_visible(const struct mywm_dock_item *it) {
    if (it->pinned) {
        return true;
    }
    return it->view != NULL && (it->view->mapped || it->view->minimized);
}

/* Хит-тест по видимой иконке дока (в координатах output layout). */
static struct mywm_dock_item *dock_item_at(struct mywm_server *server,
        double lx, double ly) {
    struct mywm_dock *dock = &server->dock;
    if (dock->tree == NULL || !dock->tree->node.enabled) {
        return NULL;
    }
    struct mywm_dock_item *it;
    wl_list_for_each(it, &dock->items, link) {
        if (!item_visible(it)) {
            continue;
        }
        if (lx >= it->lx && lx < it->lx + it->lw &&
                ly >= it->ly && ly < it->ly + it->lh) {
            return it;
        }
    }
    return NULL;
}

/*
 * Целевые размеры: непрерывный fisheye — величина роста зависит от
 * расстояния от курсора до центра иконки (гаусс), пока курсор находится
 * в полосе дока. Никаких дискретных скачков при смене "наведённой"
 * иконки: волна едет вместе с курсором.
 */
static void dock_sizes(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    struct mywm_dock_item *it;
    const int icon = server->design.dock_icon;

    double mx = server->cursor->x, my = server->cursor->y;
    bool in_band = dock->bar_w > 0 &&
        my >= dock->bar_y - DOCK_BAND_MARGIN &&
        my <= dock->bar_y + dock->bar_h + DOCK_BAND_MARGIN &&
        mx >= dock->bar_x - DOCK_MAG_SIGMA &&
        mx <= dock->bar_x + dock->bar_w + DOCK_MAG_SIGMA;

    wl_list_for_each(it, &dock->items, link) {
        if (!item_visible(it)) {
            continue;
        }
        double mag = 0.0;
        if (in_band) {
            double dx = (it->lx + it->lw / 2.0) - mx;
            mag = DOCK_MAG_EXTRA *
                exp(-(dx * dx) / (2.0 * DOCK_MAG_SIGMA * DOCK_MAG_SIGMA));
        }
        it->tw = icon + mag;
        it->th = icon + mag;
    }
}

/*
 * Таймер анимации: пружина на каждую иконку (позиция + скорость),
 * интегрирование с фактическим dt. Даёт мягкий овершут как в macOS
 * вместо линейного экспоненциального затухания.
 */
static int dock_anim_tick(void *data) {
    struct mywm_server *server = data;
    struct mywm_dock *dock = &server->dock;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = 1.0 / 60.0;
    if (dock->anim_last.tv_sec != 0 || dock->anim_last.tv_nsec != 0) {
        dt = (now.tv_sec - dock->anim_last.tv_sec) +
            (now.tv_nsec - dock->anim_last.tv_nsec) / 1e9;
    }
    dock->anim_last = now;
    if (dt > 0.05) {
        dt = 0.05;
    }
    if (dt < 1e-4) {
        dt = 1e-4;
    }

    bool moving = false;
    struct mywm_dock_item *it;
    wl_list_for_each(it, &dock->items, link) {
        /* Полускрытое интегрирование: стабильнее явного Эйлера. */
        it->vw += ((it->tw - it->cw) * DOCK_SPRING_STIFF -
                   it->vw * DOCK_SPRING_DAMP) * dt;
        it->cw += it->vw * dt;
        if (fabs(it->tw - it->cw) > 0.25 || fabs(it->vw) > 1.5) {
            moving = true;
        } else {
            it->cw = it->tw;
            it->vw = 0.0;
        }
        it->ch = it->cw;
    }

    dock_layout(server);

    if (!moving) {
        /* Сохраняем anim_running=false: следующий вызов dock_update
         * перезапустит таймер при новой цели. */
        dock->anim_running = false;
        return 0;
    }
    wl_event_source_timer_update(dock->anim_timer, DOCK_ANIM_MS);
    return 0;
}

/*
 * Пересчёт дока: полоса по центру низа layout, слева — закреплённые
 * лаунчеры, после разделителя — окна, затем свёрнутые. Иконки растут
 * вверх от нижней линии. Док виден всегда (закреплённые иконки).
 */
static void dock_layout(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    if (wlr_box_empty(&box)) {
        return;
    }

    int napps = 0, nmin = 0, npin = 0, nvis = 0;
    struct mywm_dock_item *it;
    wl_list_for_each(it, &dock->items, link) {
        if (!item_visible(it)) {
            continue;
        }
        nvis++;
        if (it->pinned) {
            npin++;
        } else if (it->view->minimized) {
            nmin++;
        } else {
            napps++;
        }
    }
    wlr_scene_node_set_enabled(&dock->tree->node, nvis > 0);
    if (nvis == 0) {
        return;
    }

    /* Проход 1: размеры (только при наведении на иконку). */
    dock_sizes(server);
    wl_list_for_each(it, &dock->items, link) {
        if (!item_visible(it)) {
            continue;
        }
        it->lw = (int)(it->cw + 0.5);
        it->lh = (int)(it->ch + 0.5);
    }

    /* Проход 2: разметка (пины | разделитель | окна | разделитель |
     * свёрнутые), по центру. */
    const struct design_config *d = &server->design;
    int total = 0;
    wl_list_for_each(it, &dock->items, link) {
        if (!item_visible(it)) {
            continue;
        }
        total += it->lw + d->dock_gap;
    }
    total -= d->dock_gap;
    bool has_sep_pin = npin > 0 && (napps + nmin) > 0;
    if (has_sep_pin) {
        total += DOCK_SEP_WIDTH + 2 * d->dock_gap;
    }
    bool has_sep = napps > 0 && nmin > 0;
    if (has_sep) {
        total += DOCK_SEP_WIDTH + 2 * d->dock_gap;
    }

    int bh = dock_height(server);
    int bar_x = box.x + (box.width - total - 2 * d->dock_pad) / 2;
    int bar_y = box.y + box.height - bh;
    wlr_scene_node_set_position(&dock->tree->node, bar_x, bar_y);
    wlr_scene_rect_set_size(dock->bar, total + 2 * d->dock_pad, bh);
    /* Прямоугольник полосы нужен fisheye-расчёту в dock_sizes. */
    dock->bar_x = bar_x;
    dock->bar_y = bar_y;
    dock->bar_w = total + 2 * d->dock_pad;
    dock->bar_h = bh;

    wlr_scene_node_set_enabled(&dock->sep->node, has_sep);
    wlr_scene_node_set_enabled(&dock->sep_pin->node, has_sep_pin);
    int x = d->dock_pad;
    bool passed_pin_sep = (npin == 0);
    bool passed_sep = (napps == 0);
    wl_list_for_each(it, &dock->items, link) {
        struct mywm_view *v = it->view;
        if (!item_visible(it)) {
            continue;
        }
        if (!passed_pin_sep && !it->pinned) {
            wlr_scene_rect_set_size(dock->sep_pin, DOCK_SEP_WIDTH,
                                    d->dock_icon);
            wlr_scene_node_set_position(&dock->sep_pin->node, x,
                                        d->dock_pad);
            x += DOCK_SEP_WIDTH + d->dock_gap;
            passed_pin_sep = true;
        }
        bool is_min = v != NULL && v->minimized;
        if (!passed_sep && is_min) {
            wlr_scene_rect_set_size(dock->sep, DOCK_SEP_WIDTH,
                                    d->dock_icon);
            wlr_scene_node_set_position(&dock->sep->node, x, d->dock_pad);
            x += DOCK_SEP_WIDTH + d->dock_gap;
            passed_sep = true;
        }
        /* Растущая иконка дополнительно приподнимается над линией дока. */
        int lift = it->lh > d->dock_icon
            ? (int)((it->lh - d->dock_icon) * 0.35)
            : 0;
        if (lift > DOCK_LIFT_MAX) {
            lift = DOCK_LIFT_MAX;
        }
        it->lx = bar_x + x;
        it->ly = bar_y + d->dock_pad + d->dock_icon - it->lh - lift;
        int iy = d->dock_pad + d->dock_icon - it->lh - lift;

        /* Показываем иконку приложения вместо прямоугольника */
        if (it->icon_img) {
            wlr_scene_node_set_enabled(&it->icon_img->node, true);
            wlr_scene_buffer_set_dest_size(it->icon_img, it->lw, it->lh);
            wlr_scene_node_set_position(&it->icon_img->node, x, iy);
            /* Прячем fallback прямоугольник */
            wlr_scene_node_set_enabled(&it->icon->node, false);
        } else {
            /* Если нет текстуры иконки, показываем fallback */
            wlr_scene_rect_set_size(it->icon, it->lw, it->lh);
            wlr_scene_node_set_position(&it->icon->node, x, iy);
            const float *color = d->dock_idle;
            if (v != NULL && v == server->focused_view) {
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
        if (it->view == focused && focused != NULL) {
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
    if (need_anim && !dock->anim_running) {
        dock->anim_running = true;
        clock_gettime(CLOCK_MONOTONIC, &dock->anim_last);
        wl_event_source_timer_update(dock->anim_timer, 1);
    }
}

void dock_refresh(struct mywm_server *server) {
    dock_layout(server);
}

/* Создание визуала иконки (fallback-прямоугольник скрыт; текстура — по
 * app_id из темы иконок, иначе цветная заглушка). */
static void dock_item_make_icons(struct mywm_server *server,
                                 struct mywm_dock_item *item,
                                 const char *app_id) {
    const struct design_config *d = &server->design;
    item->icon = wlr_scene_rect_create(server->dock.tree, d->dock_icon,
                                       d->dock_icon, d->dock_idle);
    wlr_scene_node_set_enabled(&item->icon->node, false);
    if (item->command[0] == '@' && strcmp(item->command, "@apps") == 0) {
        /* Пин меню приложений — фирменная иконка с сеткой. */
        item->icon_img = icon_create_launchpad(&server->icon_mgr,
                                               server->dock.tree,
                                               d->dock_icon);
    } else if (app_id != NULL && strlen(app_id) > 0) {
        item->icon_img = icon_load_app(&server->icon_mgr,
                                       server->dock.tree, app_id,
                                       d->dock_icon);
    }
    if (item->icon_img != NULL) {
        wlr_scene_node_set_enabled(&item->icon_img->node, false);
        wlr_scene_buffer_set_dest_size(item->icon_img, d->dock_icon,
                                       d->dock_icon);
    }
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
    server->dock.sep_pin = wlr_scene_rect_create(server->dock.tree,
                                                 DOCK_SEP_WIDTH,
                                                 d->dock_icon, d->dock_sep);
    wlr_scene_node_set_enabled(&server->dock.sep_pin->node, false);
    server->dock.dot = wlr_scene_rect_create(server->dock.tree,
                                             DOCK_DOT_W, DOCK_DOT_H,
                                             d->dock_dot);
    wlr_scene_node_set_enabled(&server->dock.dot->node, false);
    wl_list_init(&server->dock.items);

    /* Закреплённые лаунчеры — в порядке таблицы слева направо. */
    for (size_t i = 0; i < sizeof(pinned_apps) / sizeof(pinned_apps[0]);
            i++) {
        struct mywm_dock_item *item = calloc(1, sizeof(*item));
        if (item == NULL) {
            continue;
        }
        item->pinned = true;
        snprintf(item->app_id, sizeof(item->app_id), "%s",
                 pinned_apps[i].app_id);
        snprintf(item->command, sizeof(item->command), "%s",
                 pinned_apps[i].command);
        dock_item_make_icons(server, item, item->app_id);
        item->cw = item->ch = item->tw = item->th = d->dock_icon;
        wl_list_insert(server->dock.items.prev, &item->link);
    }

    struct wl_event_loop *loop =
        wl_display_get_event_loop(server->wl_display);
    server->dock.anim_timer =
        wl_event_loop_add_timer(loop, dock_anim_tick, server);
    dock_refresh(server);
}

void dock_add_view(struct mywm_server *server, struct mywm_view *view) {
    const struct design_config *d = &server->design;
    const char *app_id = NULL;
    if (view->xdg_toplevel && view->xdg_toplevel->base) {
        app_id = view->xdg_toplevel->app_id;
    }

    /* Если приложение закреплено — привязываем окно к его иконке вместо
     * создания дубликата. */
    if (app_id != NULL) {
        struct mywm_dock_item *it;
        wl_list_for_each(it, &server->dock.items, link) {
            if (it->pinned && strcasecmp(it->app_id, app_id) == 0) {
                if (it->view == NULL) {
                    it->view = view;
                    dock_refresh(server);
                    return;
                }
            }
        }
    }

    struct mywm_dock_item *item = calloc(1, sizeof(struct mywm_dock_item));
    if (item == NULL) {
        return;
    }
    item->view = view;
    dock_item_make_icons(server, item, app_id);

    item->cw = item->ch = item->tw = item->th = d->dock_icon;
    wl_list_insert(server->dock.items.prev, &item->link);
    dock_refresh(server);
}

void dock_remove_view(struct mywm_server *server, struct mywm_view *view) {
    struct mywm_dock_item *it, *tmp;
    wl_list_for_each_safe(it, tmp, &server->dock.items, link) {
        if (it->view != view) {
            continue;
        }
        if (it->pinned) {
            /* Закреплённый лаунчер остаётся, отвязываем только окно. */
            it->view = NULL;
            dock_refresh(server);
            return;
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
    struct mywm_dock_item *it = dock_item_at(server, lx, ly);
    return it != NULL ? it->view : NULL;
}

bool dock_activate_at(struct mywm_server *server, double lx, double ly) {
    struct mywm_dock_item *it = dock_item_at(server, lx, ly);
    if (it == NULL) {
        return false;
    }
    if (it->command[0] == '@') {
        if (strcmp(it->command, "@apps") == 0) {
            apps_menu_toggle(server);
        }
        return true;
    }
    if (it->pinned && it->view == NULL) {
        mywm_spawn(server, it->command);
        return true;
    }
    if (it->view == NULL) {
        return true;
    }
    focus_view(server, it->view, it->view->xdg_toplevel->base->surface);
    return true;
}

void dock_raise(struct mywm_server *server) {
    wlr_scene_node_raise_to_top(&server->dock.tree->node);
}
/* Повторное применение [design] к доку (SIGHUP): цвета панели,
 * разделителей и точки; размеры применит dock_layout. */
void dock_redesign(struct mywm_server *server) {
    struct mywm_dock *dock = &server->dock;
    if (dock->tree == NULL) {
        return;
    }
    const struct design_config *d = &server->design;
    wlr_scene_rect_set_color(dock->bar, d->dock_bg);
    wlr_scene_rect_set_color(dock->sep, d->dock_sep);
    wlr_scene_rect_set_color(dock->sep_pin, d->dock_sep);
    wlr_scene_rect_set_color(dock->dot, d->dock_dot);
    dock_refresh(server);
}
