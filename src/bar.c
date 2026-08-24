#define _GNU_SOURCE
#include "server.h"
#include <cairo.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* Верхний менюбар как в macOS: имя приложения + меню слева, системные
 * иконки и часы справа. Полупрозрачное тёмное «стекло», белый текст.
 * Цвета/шрифт/высота — в server->design ([design] в config.toml). */
#define BAR_PAD_X 12
#define BAR_APP_MAX_W 240
#define BAR_MENU_GAP 22
#define BAR_CLOCK_RIGHT 14
#define BAR_ICON_GAP 8
#define BAR_FONT_NAME 13
#define BAR_FONT_CLOCK 12
#define BAR_ICON_W 16
#define BAR_ICON_H 14
/* Позиции кнопок в баре — те же, что на заголовке окна (BTN_X из server.h). */
#define BAR_BTN_AREA_W(d) (3 * (d)->btn_size + 2 * (d)->btn_gap)

/*
 * CPU-буфер под текст: память (memfd), которую рисует cairo и которую
 * рендерер импортирует как wl_shm-буфер (get_shm / data_ptr_access).
 * Определение struct mywm_text_buf — в server.h.
 */

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static void text_buf_destroy(struct wlr_buffer *wb) {
    struct mywm_text_buf *buf = wl_container_of(wb, buf, base);
    munmap(buf->data, buf->size);
    close(buf->fd);
    free(buf);
}

static bool text_buf_get_shm(struct wlr_buffer *wb,
                             struct wlr_shm_attributes *attribs) {
    struct mywm_text_buf *buf = wl_container_of(wb, buf, base);
    attribs->fd = buf->fd;
    attribs->format = DRM_FORMAT_ARGB8888;
    attribs->width = wb->width;
    attribs->height = wb->height;
    attribs->stride = (int)buf->stride;
    attribs->offset = 0;
    return true;
}

static bool text_buf_begin_data_ptr_access(struct wlr_buffer *wb,
                                           uint32_t flags, void **data,
                                           uint32_t *format, size_t *stride) {
    (void)flags;
    struct mywm_text_buf *buf = wl_container_of(wb, buf, base);
    *data = buf->data;
    *format = DRM_FORMAT_ARGB8888;
    *stride = buf->stride;
    return true;
}

static void text_buf_end_data_ptr_access(struct wlr_buffer *wb) {
    (void)wb;
}

static const struct wlr_buffer_impl text_buf_impl = {
    .destroy = text_buf_destroy,
    .get_shm = text_buf_get_shm,
    .begin_data_ptr_access = text_buf_begin_data_ptr_access,
    .end_data_ptr_access = text_buf_end_data_ptr_access,
};

static struct mywm_text_buf *text_buf_create(int width, int height) {
    struct mywm_text_buf *buf = calloc(1, sizeof(*buf));
    if (buf == NULL) {
        return NULL;
    }
    buf->stride = (size_t)width * 4;
    buf->size = buf->stride * (size_t)height;
    buf->fd = syscall(SYS_memfd_create, "de-bar", MFD_CLOEXEC);
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
    wlr_buffer_init(&buf->base, &text_buf_impl, width, height);
    return buf;
}

static double bar_text_width(struct mywm_server *server, const char *text,
                             int px, cairo_font_weight_t weight) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(s);
    cairo_select_font_face(cr, server->design.font, CAIRO_FONT_SLANT_NORMAL,
                           weight);
    cairo_set_font_size(cr, px);
    cairo_text_extents_t e;
    cairo_text_extents(cr, text, &e);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return e.x_advance;
}

static void bar_draw(struct mywm_server *server, struct mywm_text_buf *buf,
                     const char *text, int px, cairo_font_weight_t weight) {
    const float *color = server->design.bar_text;
    int baseline = server->design.menu_bar_h - 7;
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, buf->base.width, buf->base.height,
        (int)buf->stride);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_select_font_face(cr, server->design.font, CAIRO_FONT_SLANT_NORMAL,
                           weight);
    cairo_set_font_size(cr, px);
    cairo_move_to(cr, 0, baseline);
    cairo_show_text(cr, text);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
}

/* Заменяет содержимое scene_buffer новым текстом (ширина под текст).
 * Держим собственный lock на буфер: после загрузки текстуры wlroots
 * освобождает буфер (scene_buffer->buffer = NULL), а раскладке нужна
 * ширина текста. */
static void bar_set_text(struct mywm_server *server,
                         struct wlr_scene_buffer *sb,
                         struct mywm_text_buf **slot, const char *text,
                         int px, cairo_font_weight_t weight) {
    int w = (int)bar_text_width(server, text, px, weight) + 8;
    struct mywm_text_buf *nb = text_buf_create(w, server->design.menu_bar_h);
    if (nb == NULL) {
        return;
    }
    bar_draw(server, nb, text, px, weight);
    wlr_buffer_lock(&nb->base);
    wlr_buffer_drop(&nb->base);
    wlr_scene_buffer_set_buffer(sb, &nb->base);
    if (*slot != NULL) {
        wlr_buffer_unlock(&(*slot)->base);
        *slot = NULL;
    }
    *slot = nb;
}

/* Подпись для оболочки (меню приложений): буфер с белым текстом,
 * высота по размеру шрифта. Освобождать через wlr_buffer_unlock. */
struct mywm_text_buf *shell_label_buf_weight(struct mywm_server *server,
                                             const char *text, int px,
                                             cairo_font_weight_t weight) {
    int w = (int)bar_text_width(server, text, px, weight) + 12;
    int h = px + 10;
    struct mywm_text_buf *buf = text_buf_create(w, h);
    if (buf == NULL) {
        return NULL;
    }
    const float *color = server->design.bar_text;
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, buf->base.width, buf->base.height,
        (int)buf->stride);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_select_font_face(cr, server->design.font, CAIRO_FONT_SLANT_NORMAL,
                           weight);
    cairo_set_font_size(cr, px);
    cairo_text_extents_t e;
    cairo_text_extents(cr, text, &e);
    cairo_move_to(cr, 6, h / 2.0 - e.height / 2.0 - e.y_bearing);
    cairo_show_text(cr, text);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return buf;
}

struct mywm_text_buf *shell_label_buf(struct mywm_server *server,
                                      const char *text, int px) {
    return shell_label_buf_weight(server, text, px, CAIRO_FONT_WEIGHT_NORMAL);
}

/* Пустой ARGB-буфер под cairo-рисование (пилюля ховера меню приложений).
 * Возвращается залоченным: освобождать через wlr_buffer_unlock. */
struct mywm_text_buf *shell_blank_buf(int width, int height) {
    struct mywm_text_buf *buf = text_buf_create(width, height);
    if (buf == NULL) {
        return NULL;
    }
    wlr_buffer_lock(&buf->base);
    return buf;
}

/* Обрезает заголовок до BAR_APP_MAX_W с многоточием. */
static void bar_clip_title(struct mywm_server *server, const char *title,
                           char *out, size_t out_len) {
    if (bar_text_width(server, title, BAR_FONT_NAME,
                       CAIRO_FONT_WEIGHT_BOLD) <= BAR_APP_MAX_W) {
        snprintf(out, out_len, "%s", title);
        return;
    }
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", title);
    while (tmp[0] != '\0') {
        tmp[strlen(tmp) - 1] = '\0';
        char ell[320];
        snprintf(ell, sizeof(ell), "%s…", tmp);
        if (bar_text_width(server, ell, BAR_FONT_NAME,
                           CAIRO_FONT_WEIGHT_BOLD) <= BAR_APP_MAX_W) {
            snprintf(out, out_len, "%s", ell);
            return;
        }
    }
    snprintf(out, out_len, "…");
}

static void bar_layout(struct mywm_server *server) {
    struct mywm_bar *bar = &server->bar;
    const struct design_config *d = &server->design;
    int bar_h = d->menu_bar_h;
    int btn_y = (bar_h - d->btn_size) / 2;
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    if (box.width <= 0 || box.height <= 0) {
        return;
    }
    wlr_scene_rect_set_size(bar->bg, box.width, bar_h);
    wlr_scene_rect_set_size(bar->line, box.width, 1);
    wlr_scene_node_set_position(&bar->line->node, 0, bar_h - 1);

    bool show_btns = server->focused_view != NULL &&
        server->focused_view->maximized && server->focused_view->mapped;
    for (int i = 0; i < 3; i++) {
        wlr_scene_node_set_enabled(&bar->btns[i].node->node, show_btns);
        wlr_scene_node_set_position(&bar->btns[i].node->node,
                                    BTN_X + i * (d->btn_size + d->btn_gap),
                                    btn_y);
    }

    int menus_w = bar->menus_buf != NULL ? bar->menus_buf->base.width : 0;
    int left = BAR_PAD_X +
        (show_btns ? BAR_BTN_AREA_W(d) + BTN_X : 0);
    /* Меню слева, имя приложения после них. */
    wlr_scene_node_set_position(&bar->menus->node, left, 0);
    wlr_scene_node_set_position(&bar->app->node,
                                left + menus_w + BAR_MENU_GAP, 0);
    /* Справа (как в macOS): часы у края, затем батарея и WiFi. */
    int cw = bar->clock_buf != NULL ? bar->clock_buf->base.width : 0;
    int icon_y = (bar_h - BAR_ICON_H) / 2;
    int x = box.width - BAR_CLOCK_RIGHT - cw;
    wlr_scene_node_set_position(&bar->clock->node, x, 0);
    x -= BAR_ICON_W + BAR_ICON_GAP;
    wlr_scene_node_set_position(&bar->battery->node, x, icon_y);
    x -= BAR_ICON_W + BAR_ICON_GAP;
    wlr_scene_node_set_position(&bar->wifi->node, x, icon_y);
}

/* Какая кнопка максимизированного окна в менюбаре под точкой. */
enum mywm_title_button bar_button_at(struct mywm_server *server,
                                     double lx, double ly) {
    struct mywm_bar *bar = &server->bar;
    const struct design_config *d = &server->design;
    int btn_y = (d->menu_bar_h - d->btn_size) / 2;
    if (!server->shell_cfg.builtin || bar->tree == NULL ||
        server->focused_view == NULL ||
        !server->focused_view->maximized ||
        !server->focused_view->mapped ||
        !bar->btns[0].node->node.enabled) {
        return MYWM_BTN_NONE;
    }
    double rx = lx - BTN_X;
    double ry = ly - btn_y;
    if (ry < 0 || ry >= d->btn_size || rx < 0) {
        return MYWM_BTN_NONE;
    }
    if (rx < d->btn_size) {
        return MYWM_BTN_CLOSE;
    }
    if (rx < d->btn_size + d->btn_gap) {
        return MYWM_BTN_NONE;
    }
    if (rx < 2 * d->btn_size + d->btn_gap) {
        return MYWM_BTN_MINIMIZE;
    }
    if (rx < 2 * d->btn_size + 2 * d->btn_gap) {
        return MYWM_BTN_NONE;
    }
    if (rx < 3 * d->btn_size + 2 * d->btn_gap) {
        return MYWM_BTN_MAXIMIZE;
    }
    return MYWM_BTN_NONE;
}

/* Обновляет часы в менюбаре (текст зависит от текущего времени). */
void bar_update_clock(struct mywm_server *server) {
    if (!server->shell_cfg.builtin || server->bar.tree == NULL) {
        return;
    }
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char text[48];
    if (tm_now != NULL) {
        /* Как в macOS: "чт 20 авг  21:15" (дата + время). */
        strftime(text, sizeof(text), "%a %e %b  %H:%M", tm_now);
    } else {
        snprintf(text, sizeof(text), "--:--");
    }
    bar_set_text(server, server->bar.clock, &server->bar.clock_buf,
                 text, BAR_FONT_CLOCK, CAIRO_FONT_WEIGHT_NORMAL);
    bar_layout(server);
}

static int bar_clock_tick(void *data) {
    struct mywm_server *server = data;
    bar_update_clock(server);
    wl_event_source_timer_update(server->bar.clock_timer, 1000);
    return 0;
}

/*
 * Круглая кнопка: закрашенный круг размером `size` и, если glyph != NONE,
 * глиф цвета fg поверх (×, −, ◤◢). Возвращает буфер с собственным lock.
 */
struct mywm_text_buf *mywm_button_buf_fg(int size, const float bg[4],
                                         enum mywm_title_button glyph,
                                         const float fg[4]) {
    struct mywm_text_buf *buf = text_buf_create(size, size);
    if (buf == NULL) {
        return NULL;
    }
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, size, size, (int)buf->stride);
    cairo_t *cr = cairo_create(surf);
    double c = size / 2.0;

    cairo_set_source_rgba(cr, bg[0], bg[1], bg[2], bg[3]);
    cairo_arc(cr, c, c, c - 0.5, 0, 2 * M_PI);
    cairo_fill(cr);

    if (glyph != MYWM_BTN_NONE) {
        cairo_set_source_rgba(cr, fg[0], fg[1], fg[2], fg[3]);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, 1.3);
        switch (glyph) {
        case MYWM_BTN_CLOSE:
            cairo_move_to(cr, c - 2.2, c - 2.2);
            cairo_line_to(cr, c + 2.2, c + 2.2);
            cairo_move_to(cr, c + 2.2, c - 2.2);
            cairo_line_to(cr, c - 2.2, c + 2.2);
            cairo_stroke(cr);
            break;
        case MYWM_BTN_MINIMIZE:
            cairo_move_to(cr, c - 2.4, c);
            cairo_line_to(cr, c + 2.4, c);
            cairo_stroke(cr);
            break;
        case MYWM_BTN_MAXIMIZE:
            cairo_move_to(cr, c - 0.8, c - 3.0);
            cairo_line_to(cr, c - 3.0, c - 0.8);
            cairo_line_to(cr, c - 3.0, c - 3.0);
            cairo_close_path(cr);
            cairo_fill(cr);
            cairo_move_to(cr, c + 0.8, c + 3.0);
            cairo_line_to(cr, c + 3.0, c + 0.8);
            cairo_line_to(cr, c + 3.0, c + 3.0);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        default:
            break;
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    wlr_buffer_lock(&buf->base);
    wlr_buffer_drop(&buf->base);
    return buf;
}

/* Обёртка со стандартным тёмным глифом (macOS-кнопки менюбара). */
struct mywm_text_buf *mywm_button_buf(int size, const float color[4],
                                      enum mywm_title_button glyph) {
    static const float fg[4] = {0.30f, 0.22f, 0.18f, 0.55f};
    return mywm_button_buf_fg(size, color, glyph, fg);
}

/* Полный набор кнопки: узел + обычный круг и глиф (на hover).
 * hover_bg/hover_fg — фон/цвет глифа состояния наведения (NULL — те же). */
struct mywm_btn mywm_btn_create_h(struct wlr_scene_tree *parent, int size,
                                  const float color[4],
                                  enum mywm_title_button glyph_btn,
                                  const float hover_bg[4],
                                  const float hover_fg[4]) {
    static const float default_fg[4] = {0.30f, 0.22f, 0.18f, 0.55f};
    struct mywm_btn b = {
        .plain = mywm_button_buf(size, color, MYWM_BTN_NONE),
        .glyph = mywm_button_buf_fg(size,
                                    hover_bg != NULL ? hover_bg : color,
                                    glyph_btn,
                                    hover_fg != NULL ? hover_fg
                                                     : default_fg),
    };
    b.node = wlr_scene_buffer_create(parent, NULL);
    if (b.plain != NULL) {
        wlr_scene_buffer_set_buffer(b.node, &b.plain->base);
    }
    return b;
}

struct mywm_btn mywm_btn_create(struct wlr_scene_tree *parent, int size,
                                const float color[4],
                                enum mywm_title_button glyph_btn) {
    return mywm_btn_create_h(parent, size, color, glyph_btn, NULL, NULL);
}

/* Показывает глиф (hover) или обычный круг. */
void mywm_button_hover(struct mywm_btn *btn, bool hovered) {
    if (btn == NULL || btn->plain == NULL || btn->glyph == NULL) {
        return;
    }
    struct wlr_buffer *target =
        hovered ? &btn->glyph->base : &btn->plain->base;
    if (btn->node->buffer != target) {
        wlr_scene_buffer_set_buffer(btn->node, target);
    }
}

/*
 * Пересоздание кнопки с новыми цветом/размером ([design] reload):
 * старый узел и буферы уничтожаются, новые создаются в том же дереве.
 * Позицию и enabled выставляет вызывающий.
 */
void mywm_btn_recreate(struct wlr_scene_tree *parent, struct mywm_btn *btn,
                       int size, const float color[4],
                       enum mywm_title_button glyph_btn) {
    if (btn == NULL || parent == NULL) {
        return;
    }
    if (btn->node != NULL) {
        wlr_scene_node_destroy(&btn->node->node);
    }
    if (btn->plain != NULL) {
        wlr_buffer_unlock(&btn->plain->base);
    }
    if (btn->glyph != NULL) {
        wlr_buffer_unlock(&btn->glyph->base);
    }
    btn->node = NULL;
    btn->plain = NULL;
    btn->glyph = NULL;
    *btn = mywm_btn_create(parent, size, color, glyph_btn);
}

static void bar_round_rect(cairo_t *cr, double x, double y, double w,
                           double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_arc(cr, x + w - r, y + r, r, 1.5 * M_PI, 2 * M_PI);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, 0.5 * M_PI);
    cairo_arc(cr, x + r, y + h - r, r, 0.5 * M_PI, M_PI);
    cairo_close_path(cr);
}

/* Иконки статуса как в macOS: WiFi (дуги) и батарея (контур + заливка). */
static struct mywm_text_buf *bar_icon_wifi(struct mywm_server *server) {
    const float *color = server->design.bar_icon;
    struct mywm_text_buf *buf = text_buf_create(BAR_ICON_W, BAR_ICON_H);
    if (buf == NULL) {
        return NULL;
    }
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, BAR_ICON_W, BAR_ICON_H,
        (int)buf->stride);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 1.3);
    double cx = 8.0, cy = 10.0;
    for (double r = 2.0; r <= 4.8; r += 1.4) {
        cairo_arc(cr, cx, cy, r, M_PI * 1.15, M_PI * 1.85);
        cairo_stroke(cr);
    }
    cairo_arc(cr, cx, cy, 0.9, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    wlr_buffer_lock(&buf->base);
    wlr_buffer_drop(&buf->base);
    return buf;
}

static struct mywm_text_buf *bar_icon_battery(struct mywm_server *server) {
    const float *color = server->design.bar_icon;
    struct mywm_text_buf *buf = text_buf_create(BAR_ICON_W, BAR_ICON_H);
    if (buf == NULL) {
        return NULL;
    }
    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        buf->data, CAIRO_FORMAT_ARGB32, BAR_ICON_W, BAR_ICON_H,
        (int)buf->stride);
    cairo_t *cr = cairo_create(surf);
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_set_line_width(cr, 1.2);
    /* Корпус с закруглением и «носик» справа. */
    double x = 1.5, y = 4.5, w = 12.0, h = 6.0;
    bar_round_rect(cr, x, y, w, h, 1.5);
    cairo_stroke(cr);
    bar_round_rect(cr, x + w + 0.5, y + 1.5, 2.0, h - 3.0, 0.8);
    cairo_fill(cr);
    /* Заряд ~65%. */
    bar_round_rect(cr, x + 1.5, y + 1.5, w * 0.65 - 1.5, h - 3.0, 1.0);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    wlr_buffer_lock(&buf->base);
    wlr_buffer_drop(&buf->base);
    return buf;
}

/* Обновляет имя активного приложения в менюбаре. */
void bar_update_name(struct mywm_server *server) {
    if (!server->shell_cfg.builtin || server->bar.tree == NULL) {
        return;
    }
    struct mywm_view *focused = server->focused_view;
    const char *title = "Рабочий стол";
    if (focused != NULL && focused->xdg_toplevel != NULL &&
        focused->xdg_toplevel->title != NULL &&
        focused->xdg_toplevel->title[0] != '\0') {
        title = focused->xdg_toplevel->title;
    }
    char clipped[320];
    bar_clip_title(server, title, clipped, sizeof(clipped));
    bar_set_text(server, server->bar.app, &server->bar.app_buf,
                 clipped, BAR_FONT_NAME, CAIRO_FONT_WEIGHT_BOLD);
    bar_layout(server);
}

/* Менюбар всегда поверх окон (как в macOS). */
void bar_raise(struct mywm_server *server) {
    if (server->bar.tree == NULL) {
        return;
    }
    wlr_scene_node_raise_to_top(&server->bar.tree->node);
}

void bar_init(struct mywm_server *server) {
    /* SF Pro Display из папки проекта нужен и декорациям окон
     * (заголовки), поэтому регистрируется даже без встроенного бара. */
    FcConfigAppFontAddDir(
        FcConfigGetCurrent(),
        (const FcChar8 *)"/home/temir/Проекты/Code/DE/fonts/San Francisco Pro Display");
    if (!server->shell_cfg.builtin) {
        /* Внешняя оболочка ([shell].builtin=false): менюбар не создаём. */
        return;
    }
    setlocale(LC_TIME, "");
    server->bar.server = server;
    const struct design_config *d = &server->design;
    server->bar.tree = wlr_scene_tree_create(&server->scene->tree);
    server->bar.bg = wlr_scene_rect_create(server->bar.tree, 0, 0,
                                           d->bar_bg);
    server->bar.line = wlr_scene_rect_create(server->bar.tree, 0, 1,
                                             d->bar_line);
    server->bar.app = wlr_scene_buffer_create(server->bar.tree, NULL);
    server->bar.menus = wlr_scene_buffer_create(server->bar.tree, NULL);
    server->bar.clock = wlr_scene_buffer_create(server->bar.tree, NULL);

    /* Круглые кнопки максимизированного окна (как на заголовке). */
    const float *btn_colors[3] = {d->btn_close, d->btn_minimize,
                                  d->btn_maximize};
    for (int i = 0; i < 3; i++) {
        server->bar.btns[i] = mywm_btn_create(
            server->bar.tree, d->btn_size, btn_colors[i],
            (enum mywm_title_button)(i + 1));
        wlr_scene_node_set_enabled(&server->bar.btns[i].node->node, false);
    }

    bar_set_text(server, server->bar.menus, &server->bar.menus_buf,
                 "Файл  Правка  Вид  Окно  Справка", BAR_FONT_NAME,
                 CAIRO_FONT_WEIGHT_NORMAL);

    /* Иконки статуса (как в macOS): WiFi и батарея справа. */
    server->bar.wifi_buf = bar_icon_wifi(server);
    server->bar.battery_buf = bar_icon_battery(server);
    server->bar.wifi = wlr_scene_buffer_create(server->bar.tree, NULL);
    server->bar.battery = wlr_scene_buffer_create(server->bar.tree, NULL);
    if (server->bar.wifi_buf != NULL) {
        wlr_scene_buffer_set_buffer(server->bar.wifi,
                                    &server->bar.wifi_buf->base);
    }
    if (server->bar.battery_buf != NULL) {
        wlr_scene_buffer_set_buffer(server->bar.battery,
                                    &server->bar.battery_buf->base);
    }
    struct wl_event_loop *loop =
        wl_display_get_event_loop(server->wl_display);
    server->bar.clock_timer =
        wl_event_loop_add_timer(loop, bar_clock_tick, server);
    wl_event_source_timer_update(server->bar.clock_timer, 1);

    bar_update_name(server);
}
/*
 * Повторное применение [design] к менюбару (SIGHUP): цвета полосы,
 * кнопки (пересоздание), тексты и иконки статуса перерисовываются.
 */
void bar_redesign(struct mywm_server *server) {
    const struct design_config *d = &server->design;
    struct mywm_bar *bar = &server->bar;
    if (bar->tree == NULL) {
        return;
    }
    wlr_scene_rect_set_color(bar->bg, d->bar_bg);
    wlr_scene_rect_set_color(bar->line, d->bar_line);

    const float *btn_colors[3] = {d->btn_close, d->btn_minimize,
                                  d->btn_maximize};
    for (int i = 0; i < 3; i++) {
        bool enabled = bar->btns[i].node != NULL &&
            bar->btns[i].node->node.enabled;
        mywm_btn_recreate(server->bar.tree, &server->bar.btns[i],
                          d->btn_size, btn_colors[i],
                          (enum mywm_title_button)(i + 1));
        if (bar->btns[i].node != NULL && !enabled) {
            wlr_scene_node_set_enabled(&bar->btns[i].node->node, false);
        }
    }

    /* Тексты: статичное меню, часы и имя приложения — заново новым
     * шрифтом/цветом. */
    bar_set_text(server, server->bar.menus, &server->bar.menus_buf,
                 "Файл  Правка  Вид  Окно  Справка", BAR_FONT_NAME,
                 CAIRO_FONT_WEIGHT_NORMAL);
    bar_update_clock(server);
    bar_update_name(server);

    /* Иконки статуса. */
    if (server->bar.wifi_buf != NULL) {
        wlr_buffer_unlock(&server->bar.wifi_buf->base);
        server->bar.wifi_buf = NULL;
    }
    if (server->bar.battery_buf != NULL) {
        wlr_buffer_unlock(&server->bar.battery_buf->base);
        server->bar.battery_buf = NULL;
    }
    server->bar.wifi_buf = bar_icon_wifi(server);
    server->bar.battery_buf = bar_icon_battery(server);
    if (server->bar.wifi_buf != NULL) {
        wlr_scene_buffer_set_buffer(server->bar.wifi,
                                    &server->bar.wifi_buf->base);
    }
    if (server->bar.battery_buf != NULL) {
        wlr_scene_buffer_set_buffer(server->bar.battery,
                                    &server->bar.battery_buf->base);
    }
    bar_layout(server);
}
