#define _GNU_SOURCE
#include "server.h"
#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>

/*
 * Хром окна: ОДНА CPU-текстура (рамка + заголовок + тело) с по-настоящему
 * прозрачными скруглёнными углами — четверть-окружности радиуса r в углах
 * закрашиваются альфой 0 (premultiplied). Никаких масок/шейдеров: под
 * вырезом видно обои, т.к. прозрачные пиксели не пишутся в кадр.
 *
 * Трансформации (maximize/unmaximize/genie) в стиле macOS эмулируют
 * масштаб нативными средствами wlroots 0.20 (нет scale у scene-нод):
 * тик интерполирует dest_size и позицию scene_buffer хрома и содержимого
 * вокруг якоря окна + непрозрачность.
 */

struct mywm_chrome_buf {
    struct wlr_buffer base;
    int fd;
    void *data;
    size_t size;
    size_t stride;
};

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static void chrome_buf_destroy(struct wlr_buffer *wb) {
    struct mywm_chrome_buf *buf = wl_container_of(wb, buf, base);
    munmap(buf->data, buf->size);
    close(buf->fd);
    free(buf);
}

static bool chrome_buf_get_shm(struct wlr_buffer *wb,
                               struct wlr_shm_attributes *attribs) {
    struct mywm_chrome_buf *buf = wl_container_of(wb, buf, base);
    attribs->fd = buf->fd;
    attribs->format = DRM_FORMAT_ARGB8888;
    attribs->width = wb->width;
    attribs->height = wb->height;
    attribs->stride = (int)buf->stride;
    attribs->offset = 0;
    return true;
}

static bool chrome_buf_begin_data_ptr_access(struct wlr_buffer *wb,
                                             uint32_t flags, void **data,
                                             uint32_t *format, size_t *stride) {
    (void)flags;
    struct mywm_chrome_buf *buf = wl_container_of(wb, buf, base);
    *data = buf->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buf->stride;
    return true;
}

static void chrome_buf_end_data_ptr_access(struct wlr_buffer *wb) {
    (void)wb;
}

static const struct wlr_buffer_impl chrome_buf_impl = {
    .destroy = chrome_buf_destroy,
    .get_shm = chrome_buf_get_shm,
    .begin_data_ptr_access = chrome_buf_begin_data_ptr_access,
    .end_data_ptr_access = chrome_buf_end_data_ptr_access,
};

static struct mywm_chrome_buf *chrome_buf_create(int width, int height) {
    struct mywm_chrome_buf *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        return NULL;
    }
    buf->stride = (size_t)width * 4;
    buf->size = buf->stride * (size_t)height;
    buf->fd = syscall(SYS_memfd_create, "de-chrome", MFD_CLOEXEC);
    if (buf->fd < 0) {
        free(buf);
        return NULL;
    }
    if (ftruncate(buf->fd, (off_t)buf->size) != 0) {
        close(buf->fd);
        free(buf);
        return NULL;
    }
    buf->data = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     buf->fd, 0);
    if (buf->data == MAP_FAILED) {
        close(buf->fd);
        free(buf);
        return NULL;
    }
    wlr_buffer_init(&buf->base, &chrome_buf_impl, width, height);
    return buf;
}

/* Палитра хрома — в server->design ([design] в config.toml). */

/*
 * Заливка хрома (premultiplied ARGB): рамка по контуру, заголовок сверху
 * (перекрывает верхнюю полосу рамки, как раньше rect'ы), тело — остальное.
 * В углах — четверть-окружности радиуса radius с 1px сглаживанием.
 */
static void chrome_draw(struct mywm_chrome_buf *cb, double radius,
                        const float border[4], const float title[4],
                        const float body[4], int bw, int th) {
    int w = cb->base.width;
    int h = cb->base.height;
    uint32_t *px = cb->data;
    double r = radius;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            double dx = 0.0, dy = 0.0;
            if (x < r) {
                dx = (r - 0.5) - (x + 0.5);
            } else if (x >= w - r) {
                dx = (x + 0.5) - (w - r + 0.5);
            }
            if (y < r) {
                dy = (r - 0.5) - (y + 0.5);
            } else if (y >= h - r) {
                dy = (y + 0.5) - (h - r + 0.5);
            }
            double a = 1.0;
            if (dx > 0.0 && dy > 0.0) {
                double d = sqrt(dx * dx + dy * dy);
                if (d > r) {
                    a = 0.0;
                } else if (d > r - 1.0) {
                    a = r - d;
                }
            }
            const float *c;
            if (x < bw || x >= w - bw || y >= h - bw) {
                c = border;
            } else if (y < th) {
                c = title;
            } else {
                c = body;
            }
            uint32_t v = ((uint32_t)(c[0] * a * 255.0 + 0.5) << 24) |
                         ((uint32_t)(c[1] * a * 255.0 + 0.5) << 16) |
                         ((uint32_t)(c[2] * a * 255.0 + 0.5) << 8) |
                         (uint32_t)(c[3] * a * 255.0 + 0.5);
            px[y * w + x] = v;
        }
    }
}

/*
 * Пересоздаёт текстуру хрома под текущие view->width/height. Во время
 * активной трансформации пропускается: размеры ведёт анимация, а
 * финализация (effects_tform_finalize) пересоздаст хром сама.
 */
/* Позиция кнопки i на заголовке окна (метрики из [design]). */
static int view_btn_x(const struct mywm_view *view, int i) {
    const struct design_config *d = &view->server->design;
    return d->border + BTN_X + i * (d->btn_size + d->btn_gap);
}
static int view_btn_y(const struct mywm_view *view) {
    const struct design_config *d = &view->server->design;
    return (d->title_h - d->btn_size) / 2;
}

void effects_chrome_regen(struct mywm_view *view) {
    if (view->tform_active || view->chrome == NULL) {
        return;
    }
    const struct design_config *d = &view->server->design;
    int cw = view->width + 2 * d->border;
    int ch = view->height + d->title_h + 2 * d->border;
    if (cw <= 0 || ch <= 0) {
        return;
    }
    struct mywm_chrome_buf *nb = chrome_buf_create(cw, ch);
    if (nb == NULL) {
        return;
    }
    const float *title = view->server->focused_view == view ?
        d->title_focused : d->title_unfocused;
    float border[4];
    double t = view->spr_hover.current;
    for (int i = 0; i < 4; i++) {
        border[i] = d->window_border[i] +
            (d->border_hover[i] - d->window_border[i]) * t;
    }
    chrome_draw(nb, EFFECTS_CORNER_RADIUS,
                border, title, d->window_body, d->border, d->title_h);

    wlr_buffer_lock(&nb->base);
    wlr_buffer_drop(&nb->base);
    wlr_scene_buffer_set_buffer(view->chrome, &nb->base);
    if (view->chrome_buf != NULL) {
        wlr_buffer_unlock(&view->chrome_buf->base);
    }
    view->chrome_buf = nb;
    view->chrome_w = cw;
    view->chrome_h = ch;
    wlr_scene_buffer_set_dest_size(view->chrome, cw, ch);
}

/*
 * Старт трансформации. Снимает open/close-пружины (slide/opacity/hover),
 * фиксирует стартовую геометрию (tform_a) и целевую (tform_b), запускает
 * пружину прогресса 0 -> 1. При отключённых анимациях — мгновенный snap
 * через effects_tform_finalize.
 */
void effects_tform_start(struct mywm_view *view, enum mywm_tform_kind kind) {
    struct mywm_server *server = view->server;
    if (view->closing || view->tform_active || view->chrome_buf == NULL) {
        return;
    }

    view->spr_slide.active = false;
    view->spr_slide.current = 0.0;
    view->spr_hover.active = false;
    view->spr_opacity.active = false;
    view->spr_opacity.current = 1.0;

    struct tform_geo a = {
        .x = view->x,
        .y = view->y,
        .cw = view->chrome_w,
        .ch = view->chrome_h,
        .w = view->width,
        .h = view->height,
        .op = 1.0,
    };
    struct tform_geo b = a;

    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);

    switch (kind) {
    case TFORM_MAXIMIZE: {
        const struct design_config *d = &server->design;
        double tch = box.height - d->menu_bar_h;
        b = (struct tform_geo){
            .x = box.x,
            .y = box.y + d->menu_bar_h,
            .cw = box.width,
            .ch = tch,
            .w = box.width - 2 * d->border,
            .h = tch - d->title_h - d->border,
            .op = 1.0,
        };
        break;
    }
    case TFORM_UNMAXIMIZE: {
        const struct design_config *d = &server->design;
        b = (struct tform_geo){
            .x = view->save_x,
            .y = view->save_y,
            .cw = view->save_w + 2 * d->border,
            .ch = view->save_h + d->title_h + 2 * d->border,
            .w = view->save_w,
            .h = view->save_h,
            .op = 1.0,
        };
        break;
    }
    case TFORM_GENIE_IN: {
        /* Цель — центр иконки окна в доке. */
        double cx = view->x + view->chrome_w / 2.0;
        double cy = view->y + view->chrome_h / 2.0;
        struct mywm_dock_item *it;
        wl_list_for_each(it, &server->dock.items, link) {
            if (it->view == view) {
                cx = it->lx + it->lw / 2.0;
                cy = it->ly + it->lh / 2.0;
                break;
            }
        }
        double s = EFFECTS_GENIE_SCALE;
        b = (struct tform_geo){
            .x = cx - view->chrome_w * s / 2.0,
            .y = cy - view->chrome_h * s / 2.0,
            .cw = view->chrome_w * s,
            .ch = view->chrome_h * s,
            .w = view->width * s,
            .h = view->height * s,
            .op = EFFECTS_GENIE_OPACITY,
        };
        view->tform_home = a;
        break;
    }
    case TFORM_GENIE_OUT:
        /* Обратное движение: от иконки к исходной геометрии. */
        a = view->tform_min_geo;
        b = view->tform_home;
        break;
    default:
        return;
    }

    view->tform_kind = kind;
    view->tform_a = a;
    view->tform_b = b;
    if (kind == TFORM_GENIE_IN) {
        view->tform_min_geo = b;
        wlr_scene_node_set_enabled(&view->deco_tree->node, true);
    }
    spring_init(&view->tform_spr, EFFECTS_TFORM_STIFFNESS,
                EFFECTS_TFORM_DAMPING);
    view->tform_spr.current = 0.0;
    spring_set_target(&view->tform_spr, 1.0);
    view->tform_active = true;

    if (!server->animations_cfg.enabled) {
        effects_tform_finalize(view);
        return;
    }
    effects_tform_apply(view);
    view_effects_start_anim(view);
    wlr_log(WLR_DEBUG, "tform started: view=%p kind=%d", (void *)view, kind);
}

/*
 * Интерполяция по прогрессу пружины: позиция deco_tree, dest_size хрома
 * и содержимого (масштаб вокруг якорей окна), непрозрачность. Кнопки
 * следуют за верхним левым углом хрома; при genie уменьшаются вместе
 * с окном, при максимизации сохраняют размер (как в macOS).
 */
void effects_tform_apply(struct mywm_view *view) {
    struct tform_geo *a = &view->tform_a;
    struct tform_geo *b = &view->tform_b;
    double p = view->tform_spr.current;

    double x = a->x + (b->x - a->x) * p;
    double y = a->y + (b->y - a->y) * p;
    /* Клампим: пружина при рывке dt может перекинуть цель — а wlroots
     * ассертит opacity в [0,1] и dest_size >= 0 (SIGABRT иначе). */
    double cw = fmax(1.0, a->cw + (b->cw - a->cw) * p);
    double ch = fmax(1.0, a->ch + (b->ch - a->ch) * p);
    double w = fmax(1.0, a->w + (b->w - a->w) * p);
    double h = fmax(1.0, a->h + (b->h - a->h) * p);
    double op = fmin(1.0, fmax(0.0, a->op + (b->op - a->op) * p));

    wlr_scene_node_set_position(&view->deco_tree->node, x, y);
    wlr_scene_buffer_set_dest_size(view->chrome,
                                   (int)(cw + 0.5), (int)(ch + 0.5));
    wlr_scene_node_set_position(&view->chrome->node,
                                (a->cw - cw) / 2.0, (a->ch - ch) / 2.0);
    wlr_scene_buffer_set_opacity(view->chrome, (float)op);

    if (view->content_buffer != NULL) {
        wlr_scene_buffer_set_dest_size(view->content_buffer,
                                       (int)(w + 0.5), (int)(h + 0.5));
        wlr_scene_node_set_position(&view->content_buffer->node,
                                    (a->w - w) / 2.0, (a->h - h) / 2.0);
        wlr_scene_buffer_set_opacity(view->content_buffer, (float)op);
    }

    bool genie = view->tform_kind == TFORM_GENIE_IN ||
        view->tform_kind == TFORM_GENIE_OUT;
    const struct design_config *d = &view->server->design;
    double bs = genie ? (cw / a->cw) : 1.0;
    double bx0 = (a->cw - cw) / 2.0 + view_btn_x(view, 0);
    double by0 = (a->ch - ch) / 2.0 + view_btn_y(view);
    for (int i = 0; i < 3; i++) {
        wlr_scene_node_set_position(&view->btns[i].node->node,
                                    bx0 + i *
                                        (d->btn_size + d->btn_gap) * bs, by0);
        wlr_scene_buffer_set_dest_size(view->btns[i].node,
                                       (int)(d->btn_size * bs + 0.5),
                                       (int)(d->btn_size * bs + 0.5));
        wlr_scene_buffer_set_opacity(view->btns[i].node, (float)op);
    }
}

/*
 * Завершение трансформации (settle пружины или мгновенный snap): точная
 * геометрия, возврат узлов в естественное состояние, пересоздание хрома
 * под новый размер, смена состояния окна (maximized/minimized).
 */
void effects_tform_finalize(struct mywm_view *view) {
    struct mywm_server *server = view->server;
    struct tform_geo *b = &view->tform_b;
    enum mywm_tform_kind kind = view->tform_kind;

    switch (kind) {
    case TFORM_MAXIMIZE:
        view->maximized = true;
        view->width = (int)(b->w + 0.5);
        view->height = (int)(b->h + 0.5);
        break;
    case TFORM_UNMAXIMIZE:
        view->maximized = false;
        view->width = (int)(b->w + 0.5);
        view->height = (int)(b->h + 0.5);
        break;
    case TFORM_GENIE_IN:
        view->minimized = true;
        wlr_scene_node_set_enabled(&view->deco_tree->node, false);
        if (server->focused_view == view) {
            server->focused_view = NULL;
            wlr_seat_keyboard_clear_focus(server->seat);
        }
        break;
    case TFORM_GENIE_OUT:
        view->minimized = false;
        break;
    default:
        break;
    }

    wlr_scene_node_set_position(&view->deco_tree->node, b->x, b->y);
    view->x = (int)(b->x + 0.5);
    view->y = (int)(b->y + 0.5);

    if (view->chrome_buf != NULL) {
        wlr_scene_buffer_set_dest_size(view->chrome,
                                       view->chrome_w, view->chrome_h);
        wlr_scene_node_set_position(&view->chrome->node, 0, 0);
        wlr_scene_buffer_set_opacity(view->chrome, 1.0f);
    }
    if (view->content_buffer != NULL) {
        wlr_scene_buffer_set_dest_size(view->content_buffer, 0, 0);
        wlr_scene_node_set_position(&view->content_buffer->node, 0, 0);
        wlr_scene_buffer_set_opacity(view->content_buffer, 1.0f);
    }
    for (int i = 0; i < 3; i++) {
        wlr_scene_buffer_set_dest_size(view->btns[i].node, 0, 0);
        wlr_scene_node_set_position(&view->btns[i].node->node,
                                    view_btn_x(view, i),
                                    view_btn_y(view));
        wlr_scene_buffer_set_opacity(view->btns[i].node, 1.0f);
    }
    effects_chrome_regen(view);
    view->tform_active = false;

    if (kind == TFORM_GENIE_OUT) {
        wlr_scene_node_set_enabled(&view->deco_tree->node, true);
        focus_view(server, view, view->xdg_toplevel->base->surface);
    }
    dock_refresh(server);
    bar_update_name(server);
    wlr_log(WLR_DEBUG, "tform done: view=%p kind=%d", (void *)view, kind);
}

/*
 * Отмена трансформации (например, перед анимацией закрытия): узлы
 * возвращаются в естественное состояние без смены состояния окна.
 */
void effects_tform_cancel(struct mywm_view *view) {
    if (!view->tform_active) {
        return;
    }
    view->tform_active = false;
    view->tform_spr.active = false;

    if (view->chrome_buf != NULL) {
        wlr_scene_buffer_set_dest_size(view->chrome,
                                       view->chrome_w, view->chrome_h);
        wlr_scene_node_set_position(&view->chrome->node, 0, 0);
        wlr_scene_buffer_set_opacity(view->chrome, 1.0f);
    }
    if (view->content_buffer != NULL) {
        wlr_scene_buffer_set_dest_size(view->content_buffer, 0, 0);
        wlr_scene_node_set_position(&view->content_buffer->node, 0, 0);
        wlr_scene_buffer_set_opacity(view->content_buffer, 1.0f);
    }
    for (int i = 0; i < 3; i++) {
        wlr_scene_buffer_set_dest_size(view->btns[i].node, 0, 0);
        wlr_scene_node_set_position(&view->btns[i].node->node,
                                    view_btn_x(view, i),
                                    view_btn_y(view));
        wlr_scene_buffer_set_opacity(view->btns[i].node, 1.0f);
    }
}