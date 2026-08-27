#include "server.h"
#include "effects.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/*
 * Spring-анимации в стиле macOS. Такт анимаций привязан к vblank'у:
 * output_frame_handler вызывает animations_frame_step() перед коммитом
 * каждого кадра (см. server.c), а view_effects_start_anim заказывает
 * кадры через wlr_output_schedule_frame. Сторожевой таймер страхует на
 * случай, если кадры перестают приходить (нет активных выходов).
 *
 * В wlroots 0.20 нет wlr_scene_node_set_scale/opacity (только
 * wlr_scene_buffer_set_opacity), поэтому "живость" открытия дают
 * opacity + slide по Y, а масштаб трансформаций эмулируется через
 * dest_size scene_buffer (см. effects_tform_apply).
 */

void spring_init(struct spring_anim *s, double stiffness, double damping) {
    s->stiffness = stiffness;
    s->damping = damping;
    s->current = 0.0;
    s->target = 0.0;
    s->velocity = 0.0;
    s->active = false;
}

void spring_set_target(struct spring_anim *s, double target) {
    s->target = target;
    s->active = true;
}

bool spring_step(struct spring_anim *s, double dt) {
    if (!s->active) {
        return false;
    }
    /* Явный Эйлер нестабилен при больших dt (затык event loop): без
     * клампа пружина "взрывается" и уводит opacity/размеры за ассерты
     * wlroots. */
    if (dt > 0.05) {
        dt = 0.05;
    }
    s->velocity += (s->target - s->current) * s->stiffness * dt;
    s->velocity -= s->velocity * s->damping * dt;
    s->current += s->velocity * dt;
    return true;
}

bool spring_settled(const struct spring_anim *s) {
    return !s->active ||
        (fabs(s->target - s->current) < 0.0005 && fabs(s->velocity) < 0.005);
}

/* Метрики кнопок заголовка (дубль static-хелперов из effects.c). */
static int anim_btn_x(const struct mywm_view *view, int i) {
    const struct design_config *d = &view->server->design;
    return d->border + BTN_X + i * (d->btn_size + d->btn_gap);
}
static int anim_btn_y(const struct mywm_view *view) {
    const struct design_config *d = &view->server->design;
    return (d->title_h - d->btn_size) / 2;
}

/*
 * Позиция декораций с учётом slide-анимации, opacity, zoom-масштаба
 * (open/close) и активной трансформации. Масштаб эмулируется через
 * dest_size хрома/контента/кнопок вокруг центра окна — тот же приём,
 * что в effects_tform_apply.
 */
static void apply_view_transforms(struct mywm_view *view) {
    struct spring_anim *slide = &view->spr_slide;

    if (view->tform_active) {
        effects_tform_apply(view);
        return;
    }

    wlr_scene_node_set_position(&view->deco_tree->node,
                                view->x, view->y + slide->current);
    float alpha = (float)fmax(0.0, fmin(1.0, view->spr_opacity.current));

    if (view->spr_hover.active && !view->closing) {
        /* Подсветка рамки: пересоздаём хром с интерполированным цветом
         * ДО наложения масштаба (regen сбрасывает dest_size). */
        effects_chrome_regen(view);
    }

    double sc = view->spr_scale.current;
    bool scaled = fabs(sc - 1.0) > 0.001;
    const struct design_config *d = &view->server->design;

    /* Хром: масштаб вокруг центра, в покое — естественный размер. */
    double off_x = 0.0, off_y = 0.0;
    if (view->chrome_buf != NULL) {
        int cw = view->chrome_w, chh = view->chrome_h;
        if (scaled && cw > 2 && chh > 2) {
            int dw = (int)(cw * sc + 0.5);
            int dh = (int)(chh * sc + 0.5);
            if (dw < 1) {
                dw = 1;
            }
            if (dh < 1) {
                dh = 1;
            }
            wlr_scene_buffer_set_dest_size(view->chrome, dw, dh);
            wlr_scene_node_set_position(&view->chrome->node,
                                        (cw - dw) / 2.0, (chh - dh) / 2.0);
            off_x = (double)(cw - dw) / 2.0;
            off_y = (double)(chh - dh) / 2.0;
        } else {
            wlr_scene_buffer_set_dest_size(view->chrome, cw, chh);
            wlr_scene_node_set_position(&view->chrome->node, 0, 0);
        }
        wlr_scene_buffer_set_opacity(view->chrome, alpha);
    }

    /* Контент клиента: масштаб синхронно с хромом. */
    if (view->content_buffer != NULL) {
        int w = view->width, h = view->height;
        if (scaled && w > 2 && h > 2) {
            int dw = (int)(w * sc + 0.5);
            int dh = (int)(h * sc + 0.5);
            if (dw < 1) {
                dw = 1;
            }
            if (dh < 1) {
                dh = 1;
            }
            wlr_scene_buffer_set_dest_size(view->content_buffer, dw, dh);
            wlr_scene_node_set_position(&view->content_buffer->node,
                                        (w - dw) / 2.0, (h - dh) / 2.0);
        } else {
            wlr_scene_buffer_set_dest_size(view->content_buffer, 0, 0);
            wlr_scene_node_set_position(&view->content_buffer->node, 0, 0);
        }
        wlr_scene_buffer_set_opacity(view->content_buffer, alpha);
    }

    /* Тень: масштабируется вместе с окном, в покое — естественный размер.
     * Отдельно от хрома: при закрытии контент/хром могут быть уже NULL. */
    if (scaled && view->chrome_w > 2 && view->chrome_h > 2) {
        const int m = EFFECTS_SHADOW_MARGIN;
        int nw = view->chrome_w + 2 * m;
        int nh = view->chrome_h + 2 * m;
        int sdw = (int)(nw * sc + 0.5);
        int sdh = (int)(nh * sc + 0.5);
        effects_shadow_place(view,
                             (view->chrome_w - sdw) / 2,
                             (view->chrome_h - sdh) / 2 +
                                 (int)(EFFECTS_SHADOW_BIAS * sc),
                             sdw, sdh, alpha);
    } else {
        effects_shadow_reset_alpha(view, alpha);
    }

    /* Кнопки: сжимаются вместе с окном (как при genie), в покое —
     * натуральный размер. */
    for (int i = 0; i < 3; i++) {
        if (view->btns[i].node == NULL) {
            continue;
        }
        double bs = scaled ? sc : 1.0;
        wlr_scene_node_set_position(&view->btns[i].node->node,
                                    off_x + anim_btn_x(view, i) * bs,
                                    off_y + anim_btn_y(view) * bs);
        if (scaled) {
            int bsz = (int)(d->btn_size * bs + 0.5);
            if (bsz < 1) {
                bsz = 1;
            }
            wlr_scene_buffer_set_dest_size(view->btns[i].node, bsz, bsz);
        } else {
            wlr_scene_buffer_set_dest_size(view->btns[i].node, 0, 0);
        }
        wlr_scene_buffer_set_opacity(view->btns[i].node, alpha);
    }

    /* Заголовок масштабируется и гаснет вместе с окном. */
    if (scaled) {
        view_title_apply_anim(view, sc, off_x, off_y, alpha);
    } else {
        view_title_reset_anim(view, alpha);
    }
}

/* Есть ли хотя бы одно окно с активной анимацией. */
static bool animations_active(struct mywm_server *server) {
    struct mywm_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->anim_active) {
            return true;
        }
    }
    return false;
}

/*
 * Шаг всех активных пружин с фактическим dt (с клампом). Когда пружины
 * view затухли: завершаем трансформацию, либо (при закрытии) выполняем
 * финальный destroy. Возвращает true, если анимации ещё идут.
 *
 * Основной такт — vblank (animations_frame_step из output_frame_handler):
 * обновления попадают ровно на кадр развёртки. Сторожевой таймер
 * (animation_tick) продвигает анимации только если кадры перестали
 * приходить (нет активных выходов).
 */
static bool animation_step_all(struct mywm_server *server) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Защита от двойного тика (близкие flip'ы нескольких мониторов):
     * слишком частые шаги не меняют картинку, но гоняют regen хрома. */
    if (server->anim_last.tv_sec != 0 || server->anim_last.tv_nsec != 0) {
        double since = (now.tv_sec - server->anim_last.tv_sec) +
            (now.tv_nsec - server->anim_last.tv_nsec) / 1e9;
        if (since < EFFECTS_ANIM_MIN_STEP_MS / 1000.0) {
            return animations_active(server);
        }
    }

    double dt = 1.0 / 60.0;
    if (server->anim_last.tv_sec != 0 || server->anim_last.tv_nsec != 0) {
        dt = (now.tv_sec - server->anim_last.tv_sec) +
            (now.tv_nsec - server->anim_last.tv_nsec) / 1e9;
    }
    server->anim_last = now;
    if (dt < 1e-4) {
        dt = 1e-4;
    }
    /* Сторожевой тик может прийти через большой интервал — клампим,
     * иначе анимации «перепрыгивают» кусок пути за один шаг. */
    if (dt > 1.0 / 10.0) {
        dt = 1.0 / 10.0;
    }

    bool any_active = false;
    struct mywm_view *view, *tmp;
    wl_list_for_each_safe(view, tmp, &server->views, link) {
        if (!view->anim_active) {
            continue;
        }
        spring_step(&view->spr_slide, dt);
        spring_step(&view->spr_opacity, dt);
        spring_step(&view->spr_hover, dt);
        spring_step(&view->spr_scale, dt);
        bool tform_settled = true;
        if (view->tform_active) {
            /* Временной прогресс с easing вместо пружины: сворачивание
             * — плавный in-out (втягивание в док), максимизация/выход
             * из дока — быстрый старт и мягкая посадка (out-quint). */
            view->tform_elapsed += dt * 1000.0;
            double raw = view->tform_duration > 0.0
                ? fmin(1.0, view->tform_elapsed / view->tform_duration)
                : 1.0;
            double p;
            if (view->tform_kind == TFORM_GENIE_IN) {
                /* easeInOutCubic */
                p = raw < 0.5 ? 4.0 * raw * raw * raw
                              : 1.0 - pow(-2.0 * raw + 2.0, 3.0) / 2.0;
            } else {
                /* easeOutQuint */
                double q = 1.0 - raw;
                p = 1.0 - q * q * q * q * q;
            }
            view->tform_spr.current = p;
            tform_settled = raw >= 1.0;
        }
        apply_view_transforms(view);

        if (!spring_settled(&view->spr_slide) ||
                !spring_settled(&view->spr_opacity) ||
                !spring_settled(&view->spr_hover) ||
                !spring_settled(&view->spr_scale) ||
                !tform_settled) {
            any_active = true;
            continue;
        }
        if (view->tform_active) {
            effects_tform_finalize(view);
        }
        if (view->closing) {
            /* Анимация закрытия завершена — теперь можно удалить view. */
            wlr_log(WLR_DEBUG, "close animation done: view=%p",
                    (void *)view);
            view_destroy_final(view);
            continue;
        }
        /* Доводим до точных значений и помечаем view неактивным. */
        apply_view_transforms(view);
        view->anim_active = false;
        wlr_log(WLR_DEBUG, "animations settled: view=%p", (void *)view);
    }

    return any_active;
}

/* Сторожевой таймер: основной такт даёт vblank. Если кадры не идут
 * дольше EFFECTS_ANIM_WATCHDOG_MS — продвигаем анимации отсюда, чтобы
 * окна не зависали в середине пружины. */
static int animation_tick(void *data) {
    struct mywm_server *server = data;

    bool any_active = animation_step_all(server);
    if (!any_active) {
        if (server->anim_timer != NULL) {
            wl_event_source_remove(server->anim_timer);
            server->anim_timer = NULL;
        }
        return 0;
    }
    wl_event_source_timer_update(server->anim_timer,
                                 EFFECTS_ANIM_WATCHDOG_MS);
    return 0;
}

bool animations_frame_step(struct mywm_server *server) {
    bool any_active = animations_active(server);
    if (!any_active) {
        /* Анимации закончились — сторожевой таймер больше не нужен. */
        if (server->anim_timer != NULL) {
            wl_event_source_remove(server->anim_timer);
            server->anim_timer = NULL;
        }
        return false;
    }
    return animation_step_all(server);
}

void animations_init(struct mywm_server *server) {
    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    /* Создаём таймер сразу: он запускается по требованию (anim_active). */
    server->anim_timer = wl_event_loop_add_timer(loop, animation_tick, server);
    if (server->anim_timer == NULL) {
        wlr_log(WLR_ERROR, "animations: failed to create timer");
    }
}

void animations_finish(struct mywm_server *server) {
    if (server->anim_timer != NULL) {
        wl_event_source_remove(server->anim_timer);
        server->anim_timer = NULL;
    }
}

void view_effects_start_anim(struct mywm_view *view) {
    if (view->anim_active) {
        return;
    }
    view->anim_active = true;
    if (view->server->anim_timer == NULL) {
        struct wl_event_loop *loop =
            wl_display_get_event_loop(view->server->wl_display);
        /* Сторожевой таймер: основной такт — vblank, таймер страхует. */
        view->server->anim_timer =
            wl_event_loop_add_timer(loop, animation_tick, view->server);
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    view->server->anim_last = now;
    wl_event_source_timer_update(view->server->anim_timer,
                                 EFFECTS_ANIM_WATCHDOG_MS);

    /* Заказать кадры на всех выходах: такт анимаций пойдёт от
     * output_frame_handler ровно в такт развёртки монитора. */
    struct mywm_output *o;
    wl_list_for_each(o, &view->server->outputs, link) {
        wlr_output_schedule_frame(o->wlr_output);
    }
}

void view_effects_stop_anim(struct mywm_view *view) {
    view->anim_active = false;
    view->spr_slide.active = false;
    view->spr_opacity.active = false;
    view->spr_hover.active = false;
    view->spr_scale.active = false;
    view->spr_scale.current = 1.0;
}

/*
 * Анимация открытия окна (map): fade-in содержимого + slide снизу вверх.
 * Если анимации отключены в конфиге — мгновенный snap.
 */
void view_effects_open(struct mywm_view *view) {
    struct mywm_server *server = view->server;

    spring_init(&view->spr_opacity, server->animations_cfg.stiffness,
                server->animations_cfg.damping);
    spring_init(&view->spr_slide, server->animations_cfg.stiffness,
                server->animations_cfg.damping);
    spring_init(&view->spr_hover, EFFECTS_HOVER_STIFFNESS,
                EFFECTS_HOVER_DAMPING);
    spring_init(&view->spr_scale, server->animations_cfg.stiffness,
                server->animations_cfg.damping);
    view->closing = false;

    if (!server->animations_cfg.enabled) {
        view->spr_opacity.current = 1.0;
        view->spr_slide.current = 0.0;
        apply_view_transforms(view);
        wlr_log(WLR_DEBUG, "open: instant (animations off): view=%p",
                (void *)view);
        return;
    }

    view->spr_opacity.current = 0.0;
    view->spr_slide.current = server->animations_cfg.open_slide;
    /* Zoom как в macOS: окно раскрывается из slightly меньшего масштаба. */
    view->spr_scale.current = EFFECTS_OPEN_SCALE;
    spring_set_target(&view->spr_opacity, 1.0);
    spring_set_target(&view->spr_slide, 0.0);
    spring_set_target(&view->spr_scale, 1.0);
    apply_view_transforms(view);
    view_effects_start_anim(view);
    wlr_log(WLR_DEBUG, "open animation started: view=%p", (void *)view);
}

/*
 * Анимация закрытия (destroy): fade-out хрома + slide вверх. Реальное
 * удаление view откладывается до затухания пружин (view_destroy_final
 * вызывается из тика). При отключённых анимациях удаляем сразу.
 */
void view_effects_close(struct mywm_view *view) {
    struct mywm_server *server = view->server;

    if (view->anim_active) {
        view_effects_stop_anim(view);
    }
    effects_tform_cancel(view);
    if (!server->animations_cfg.enabled) {
        wlr_log(WLR_DEBUG, "close: instant (animations off): view=%p",
                (void *)view);
        view_destroy_final(view);
        return;
    }

    view->closing = true;
    spring_set_target(&view->spr_opacity, 0.0);
    spring_set_target(&view->spr_slide, -server->animations_cfg.close_slide);
    spring_set_target(&view->spr_hover, 0.0);
    /* Лёгкое сжатие при растворении (zoom-out macOS). */
    if (view->spr_scale.current >= 0.999) {
        view->spr_scale.current = 1.0;
    }
    spring_set_target(&view->spr_scale, EFFECTS_CLOSE_SCALE);
    view_effects_start_anim(view);
    wlr_log(WLR_DEBUG, "close animation started: view=%p", (void *)view);
}

/* Hover: пружина подсветки рамки (быстрая, отзывчивая). */
void view_effects_hover(struct mywm_view *view, bool hovered) {
    if (view->hovered == hovered) {
        return;
    }
    view->hovered = hovered;
    spring_set_target(&view->spr_hover, hovered ? 1.0 : 0.0);
    view_effects_start_anim(view);
    wlr_log(WLR_DEBUG, "hover: view=%p state=%d", (void *)view, hovered);
}

/* Базовая позиция окна; slide применяется анимацией поверх. */
void effects_view_set_position(struct mywm_view *view, int x, int y) {
    view->x = x;
    view->y = y;
    wlr_scene_node_set_position(&view->deco_tree->node,
                                x, y + view->spr_slide.current);
}