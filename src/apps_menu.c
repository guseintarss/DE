#define _GNU_SOURCE
#include "server.h"
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/*
 * Полноэкранное меню приложений (Launchpad): тёмный оверлей поверх рабочего
 * стола с сеткой иконок и подписей. Список строится из .desktop-файлов
 * XDG-каталогов при каждом открытии. Приложения, не влезающие в сетку,
 * образуют страницы: колесо/свайп/клик по точкам листает их с плавной
 * анимацией горизонтального скольжения (как Launchpad в macOS).
 * Клик по иконке — запуск, клик мимо / Esc / повторный клик по пину —
 * закрытие.
 */

#define APPS_MAX 256
#define APPS_CELL_W 150
#define APPS_ICON 68
#define APPS_LABEL_PX 12
#define APPS_TOP_PAD 60
#define APPS_BOTTOM_PAD 40
#define APPS_FADE_IN_S 0.18    /* Появление оверлея */
#define APPS_FADE_OUT_S 0.12   /* Быстрое растворение при закрытии */
#define APPS_FADE_MS 16
#define APPS_HOVER_SCALE 0.14  /* Прибавка масштаба иконки под курсором */
#define APPS_HOVER_LIFT 6      /* Подъём иконки, px */
#define APPS_PILL_ALPHA 0.10   /* Альфа пилюли-подсветки */
#define APPS_FLIP_S 0.42       /* Длительность перелистывания, с */
#define APPS_PAGE_GAP 96       /* Зазор между страницами при листании */
#define APPS_FLIP_MS 16
#define APPS_DOT_STEP 16       /* Шаг точек-индикатора страниц */
#define APPS_DOT_R 4.0         /* Радиус точки */
#define APPS_SCROLL_THRESHOLD 1.5 /* Накопленная дельта для листания */
#define APPS_SCROLL_IDLE_S 0.35   /* Пауза сбрасывает накопление */
#define APPS_SWIPE_GAIN 4.0    /* Пиксели пальца -> доли страницы */
#define APPS_SWIPE_END_MS 140  /* Тишина = свайп закончен */
#define APPS_SWIPE_RUBBER 0.3  /* Жёсткость «резины» за краями */

struct app_entry {
    char *name;
    char *exec;
    char *icon;
};

struct apps_cell {
    struct wl_list link;
    struct app_entry app;
    struct wlr_scene_buffer *icon_node;
    struct wlr_scene_buffer *label_node;
    struct mywm_text_buf *label_buf;
    int lx, ly, lw, lh;
    int page;                      /* Страница, на которой лежит ячейка */
    /* Ховер: hs — текущий прогресс 0..1 (0 нет, 1 под курсором),
     * ht — целевое значение; анимируются в hover_tick. */
    float hs, ht;
};

struct mywm_apps_menu {
    struct mywm_server *server;
    struct wlr_scene_tree *tree;
    /* Фон — текстура (не scene_rect): у буфера есть opacity для фейда. */
    struct wlr_scene_buffer *bg;
    /* Пилюля-подсветка под ячейкой, на которой курсор. */
    struct wlr_scene_buffer *hover_bg;
    struct mywm_text_buf *hover_tex;
    struct apps_cell *hover_cell;
    float pill_a;                  /* текущая альфа пилюли 0..1 */
    int pill_w, pill_h;
    double hov_last;
    struct wl_event_source *hov_timer;
    /* Постраничный режим: страница, их число, ячеек на страницу. */
    int page, pages, per_page;
    /* Анимация перелистывания: from -> to с прогрессом flip_t 0..1.
     * flip_pending — страница, запрошенная колесом во время анимации. */
    bool flipping;
    int flip_dir;                  /* +1 вперёд, -1 назад */
    int flip_from, flip_to, flip_pending;
    double flip_t, flip_last;
    struct wl_event_source *flip_timer;
    /* Накопитель колеса: страница листается, когда сумма дельт
     * превышает порог; пауза сбрасывает сумму. */
    double scroll_acc, scroll_last;
    /* Свайп двумя пальцами: страницы тянутся за пальцем, при
     * остановке — доводка до ближайшей страницы анимацией. */
    bool swiping;
    int swipe_base;                /* Страница, от которой тянем */
    double swipe_off;              /* Смещение в долях страницы */
    struct wl_event_source *swipe_timer;
    /* Точки-индикаторы страниц под сеткой. */
    struct wlr_scene_buffer *dots;
    struct mywm_text_buf *dots_tex;
    int dots_x, dots_y, dots_w, dots_h;
    struct wl_list cells;
    size_t count;
    bool open;
    int scroll;
    /* Анимация появления/закрытия. */
    struct wl_event_source *fade_timer;
    struct timespec fade_last;
    double fade;                   /* 0..1 */
    int fade_dir;                  /* 1 открытие, -1 закрытие, 0 покой */
    /* Геометрия сетки (в координатах дерева меню = layout - origin). */
    int cols, rows_visible, cell_h, grid_x, grid_top;
};

static void entry_clear(struct app_entry *e) {
    free(e->name);
    free(e->exec);
    free(e->icon);
}

/* Убирает коды полей desktop-файла из Exec: %f %F %u %U %d %D %n %N %i %c %k %v %m. */
static void exec_strip_codes(const char *in, char *out, size_t out_len) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; i++) {
        if (in[i] == '%' && in[i + 1] != '\0') {
            i++;
            continue;
        }
        out[o++] = in[i];
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) {
        o--;
    }
    out[o] = '\0';
}

/* Разбор одного .desktop файла. Возвращает true, если приложение годно. */
static bool parse_desktop(const char *path, struct app_entry *e) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return false;
    }
    char line[1024];
    char name[512] = "", exec[512] = "", icon[256] = "";
    bool in_entry = false, ok_type = false, hidden = false, no_display = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (line[0] == '[') {
            in_entry = strncmp(line, "[Desktop Entry]", 15) == 0;
            continue;
        }
        if (!in_entry || line[0] == '#' || strchr(line, '=') == NULL) {
            continue;
        }
        char *eq = strchr(line, '=');
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "Name") == 0 && name[0] == '\0') {
            snprintf(name, sizeof(name), "%s", val);
        } else if (strcmp(key, "Exec") == 0) {
            snprintf(exec, sizeof(exec), "%s", val);
        } else if (strcmp(key, "Icon") == 0) {
            snprintf(icon, sizeof(icon), "%s", val);
        } else if (strcmp(key, "Type") == 0) {
            ok_type = strcmp(val, "Application") == 0;
        } else if (strcmp(key, "Hidden") == 0) {
            hidden = strcmp(val, "true") == 0;
        } else if (strcmp(key, "NoDisplay") == 0) {
            no_display = strcmp(val, "true") == 0;
        }
    }
    fclose(f);
    if (!ok_type || hidden || no_display || name[0] == '\0' ||
            exec[0] == '\0') {
        return false;
    }
    memset(e, 0, sizeof(*e));
    e->name = strdup(name);
    e->icon = strdup(icon);
    e->exec = malloc(strlen(exec) + 1);
    if (e->name == NULL || e->icon == NULL || e->exec == NULL) {
        entry_clear(e);
        return false;
    }
    exec_strip_codes(exec, e->exec, strlen(exec) + 1);
    return e->exec[0] != '\0';
}

static void scan_dir(const char *dir, struct app_entry *apps, size_t *count) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL && *count < APPS_MAX) {
        const char *ext = strrchr(de->d_name, '.');
        if (ext == NULL || strcmp(ext, ".desktop") != 0) {
            continue;
        }
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct app_entry e;
        if (!parse_desktop(path, &e)) {
            continue;
        }
        bool dup = false;
        for (size_t i = 0; i < *count; i++) {
            if (strcasecmp(apps[i].exec, e.exec) == 0 &&
                    strcasecmp(apps[i].name, e.name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            entry_clear(&e);
            continue;
        }
        apps[(*count)++] = e;
    }
    closedir(d);
}

static int cmp_entry(const void *a, const void *b) {
    const struct app_entry *ea = a, *eb = b;
    return strcasecmp(ea->name, eb->name);
}

static void cells_destroy(struct mywm_apps_menu *m) {
    struct apps_cell *cell, *tmp;
    wl_list_for_each_safe(cell, tmp, &m->cells, link) {
        wlr_scene_node_destroy(&cell->icon_node->node);
        if (cell->label_node != NULL) {
            wlr_scene_node_destroy(&cell->label_node->node);
        }
        if (cell->label_buf != NULL) {
            wlr_buffer_unlock(&cell->label_buf->base);
        }
        wl_list_remove(&cell->link);
        entry_clear(&cell->app);
        free(cell);
    }
    if (m->hover_bg != NULL) {
        wlr_scene_node_destroy(&m->hover_bg->node);
        m->hover_bg = NULL;
    }
    if (m->hover_tex != NULL) {
        wlr_buffer_unlock(&m->hover_tex->base);
        m->hover_tex = NULL;
    }
    m->hover_cell = NULL;
    m->pill_a = 0.0f;
    m->hov_last = 0.0;
    m->count = 0;
    /* Сброс постраничного состояния. */
    if (m->flip_timer != NULL) {
        wl_event_source_timer_update(m->flip_timer, 0);
    }
    m->flipping = false;
    m->page = 0;
    m->pages = 0;
    m->per_page = 0;
    m->flip_t = 0.0;
    m->flip_pending = -1;
    if (m->swipe_timer != NULL) {
        wl_event_source_timer_update(m->swipe_timer, 0);
    }
    m->swiping = false;
    m->swipe_off = 0.0;
    m->scroll_acc = 0.0;
    m->scroll_last = 0.0;
    if (m->dots != NULL) {
        wlr_scene_node_destroy(&m->dots->node);
        m->dots = NULL;
    }
    if (m->dots_tex != NULL) {
        wlr_buffer_unlock(&m->dots_tex->base);
        m->dots_tex = NULL;
    }
    m->dots_w = 0;
    m->dots_h = 0;
}

static void hover_apply_alpha(struct mywm_apps_menu *m);

/* Применяет текущую прозрачность к фону и всем видимым ячейкам.
 * ВАЖНО: wlr_scene_buffer_set_opacity принимает float 0..1. */
static void menu_apply_fade(struct mywm_apps_menu *m) {
    float alpha = (float)m->fade;
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    if (m->bg != NULL) {
        wlr_scene_buffer_set_opacity(m->bg, alpha);
    }
    if (m->dots != NULL && m->dots->node.enabled) {
        wlr_scene_buffer_set_opacity(m->dots, alpha);
    }
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        if (!cell->icon_node->node.enabled) {
            continue;
        }
        wlr_scene_buffer_set_opacity(cell->icon_node, alpha);
        if (cell->label_node != NULL) {
            wlr_scene_buffer_set_opacity(cell->label_node, alpha);
        }
    }
    /* Пилюля живёт в той же альфе фейда. */
    struct mywm_apps_menu *mm = m;
    hover_apply_alpha(mm);
}

static void menu_fade_start(struct mywm_apps_menu *m, int dir) {
    m->fade_dir = dir;
    clock_gettime(CLOCK_MONOTONIC, &m->fade_last);
    if (m->fade_timer != NULL) {
        wl_event_source_timer_update(m->fade_timer, 1);
    }
}

/* Тик фейда: линейное затухание/появление; по завершении закрытия
 * прячет дерево и уничтожает ячейки. */
static int menu_fade_tick(void *data) {
    struct mywm_apps_menu *m = data;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = 1.0 / 60.0;
    if (m->fade_last.tv_sec != 0 || m->fade_last.tv_nsec != 0) {
        dt = (now.tv_sec - m->fade_last.tv_sec) +
            (now.tv_nsec - m->fade_last.tv_nsec) / 1e9;
    }
    m->fade_last = now;
    if (dt > 0.05) {
        dt = 0.05;
    }

    double speed = m->fade_dir > 0 ? 1.0 / APPS_FADE_IN_S
                                   : -1.0 / APPS_FADE_OUT_S;
    m->fade += speed * dt;
    bool done = true;
    if (m->fade >= 1.0 && m->fade_dir > 0) {
        m->fade = 1.0;
        m->fade_dir = 0;
    } else if (m->fade <= 0.0 && m->fade_dir < 0) {
        m->fade = 0.0;
        m->fade_dir = 0;
        wlr_scene_node_set_enabled(&m->tree->node, false);
        cells_destroy(m);
    } else {
        done = false;
    }
    menu_apply_fade(m);

    if (!done) {
        wl_event_source_timer_update(m->fade_timer, APPS_FADE_MS);
    }
    return 0;
}

/* --- Ховер ячеек: пилюля-подсветка + масштаб/подъём иконки --- */

/* Применяет прогресс ховера s (0..1) к геометрии иконки ячейки. */
static void cell_apply_hover(struct apps_cell *cell, float s) {
    if (!cell->icon_node->node.enabled) {
        return;
    }
    double sc = 1.0 + APPS_HOVER_SCALE * s;
    int size = (int)(APPS_ICON * sc + 0.5);
    wlr_scene_buffer_set_dest_size(cell->icon_node, size, size);
    int lift = (int)(-APPS_HOVER_LIFT * s + 0.5);
    /* Позиция из relayout + центрирование увеличенной иконки. */
    int base_x = cell->lx + (APPS_CELL_W - APPS_ICON) / 2;
    int grow = (size - APPS_ICON) / 2;
    wlr_scene_node_set_position(&cell->icon_node->node,
                                base_x - grow, cell->ly - grow + lift);
}

/* Ставит пилюлю под текущую hovered-ячейку. */
static void hover_pill_place(struct mywm_apps_menu *m) {
    if (m->hover_bg == NULL) {
        return;
    }
    struct apps_cell *c = m->hover_cell;
    if (c == NULL || !c->icon_node->node.enabled) {
        wlr_scene_node_set_enabled(&m->hover_bg->node, false);
        return;
    }
    wlr_scene_node_set_enabled(&m->hover_bg->node, true);
    wlr_scene_node_set_position(&m->hover_bg->node, c->lx + 9, c->ly + 4);
}

/* Альфа пилюли = фейд меню * собственная анимация появления. */
static void hover_apply_alpha(struct mywm_apps_menu *m) {
    if (m->hover_bg != NULL) {
        float a = (float)(m->fade * m->pill_a);
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        wlr_scene_buffer_set_opacity(m->hover_bg, a);
    }
}

/* Тик: экспоненциальное сближение hs->ht у всех анимируемых ячеек и
 * альфы пилюли; таймер останавливается, когда всё сошлось. */
static int hover_tick(void *data) {
    struct mywm_apps_menu *m = data;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    double dt = 1.0 / 60.0;
    if (m->hov_last > 0.0) {
        dt = now - m->hov_last;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.05) dt = 0.05;
    }
    m->hov_last = now;

    double k = dt * 14.0;
    if (k > 1.0) k = 1.0;
    bool busy = false;

    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        if (cell->hs == cell->ht) {
            continue;
        }
        cell->hs += (float)((cell->ht - cell->hs) * k);
        if (cell->hs < 0.002f && cell->ht == 0.0f) cell->hs = 0.0f;
        if (cell->hs > 0.998f && cell->ht == 1.0f) cell->hs = 1.0f;
        cell_apply_hover(cell, cell->hs);
        busy |= cell->hs != cell->ht;
    }

    float pt = m->hover_cell != NULL ? 1.0f : 0.0f;
    if (m->pill_a != pt) {
        m->pill_a += (float)((pt - m->pill_a) * k);
        if (m->pill_a < 0.004f && pt == 0.0f) m->pill_a = 0.0f;
        if (m->pill_a > 0.996f && pt == 1.0f) m->pill_a = 1.0f;
        hover_apply_alpha(m);
        hover_pill_place(m);
        busy |= m->pill_a != pt;
    }

    if (!busy && m->hov_timer != NULL) {
        wl_event_source_timer_update(m->hov_timer, 0);
        m->hov_last = 0.0;
    } else if (busy) {
        wl_event_source_timer_update(m->hov_timer, APPS_FADE_MS);
    }
    return 0;
}

static void hover_kick(struct mywm_apps_menu *m) {
    if (m->hov_timer != NULL) {
        wl_event_source_timer_update(m->hov_timer, 1);
    }
}

/* Мгновенный сброс ховера (перед перелистыванием): пока страницы
 * летят, тик ховера не должен трогать геометрию иконок. */
static void hover_hard_reset(struct mywm_apps_menu *m) {
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        cell->hs = 0.0f;
        cell->ht = 0.0f;
    }
    m->hover_cell = NULL;
    m->pill_a = 0.0f;
    hover_apply_alpha(m);
    if (m->hov_timer != NULL) {
        wl_event_source_timer_update(m->hov_timer, 0);
    }
    m->hov_last = 0.0;
}

/* Базовая геометрия ячейки (без ховера и переворота). */
static void cell_place_base(struct apps_cell *cell) {
    wlr_scene_buffer_set_dest_size(cell->icon_node, APPS_ICON, APPS_ICON);
    wlr_scene_node_set_position(&cell->icon_node->node,
                                cell->lx +
                                    (APPS_CELL_W - APPS_ICON) / 2,
                                cell->ly);
    if (cell->label_node != NULL && cell->label_buf != NULL) {
        wlr_scene_node_set_position(
            &cell->label_node->node,
            cell->lx + (APPS_CELL_W -
                        (int)cell->label_buf->base.width) / 2,
            cell->ly + APPS_ICON + 4);
    }
}

/* --- Листание страниц скольжением (Launchpad) --- */

static void flip_start(struct mywm_apps_menu *m, int dir, int target);
static void flip_launch(struct mywm_apps_menu *m, int from, int to,
                        int dir, double t0);
static void dots_update(struct mywm_apps_menu *m);

/* Сдвиг ячейки на dx по горизонтали (скольжение страницы). Иконки
 * остаются в базовом масштабе, прозрачность = фейд меню. */
static void cell_shift(struct apps_cell *c, double dx) {
    wlr_scene_buffer_set_dest_size(c->icon_node, APPS_ICON, APPS_ICON);
    wlr_scene_node_set_position(&c->icon_node->node,
                                c->lx + (APPS_CELL_W - APPS_ICON) / 2 +
                                    (int)(dx + (dx >= 0 ? 0.5 : -0.5)),
                                c->ly);
    if (c->label_node != NULL && c->label_buf != NULL) {
        wlr_scene_node_set_position(
            &c->label_node->node,
            c->lx + (APPS_CELL_W -
                     (int)c->label_buf->base.width) / 2 +
                (int)(dx + (dx >= 0 ? 0.5 : -0.5)),
            c->ly + APPS_ICON + 4);
    }
}

/* Рендер скольжения между парой страниц с линейным прогрессом p
 * (0..1): обе страницы едут вместе как одна лента — уходящая уходит
 * против направления на ширину сетки, приходящая приходит из-за края.
 * Используется и таймером анимации (с easing), и свайпом (линейно). */
static void flip_render(struct mywm_apps_menu *m, int from, int to,
                        int dir, double p) {
    if (p < 0.0) {
        p = 0.0;
    }
    if (p > 1.0) {
        p = 1.0;
    }
    /* Шаг ленты = ширина страницы + зазор: соседние страницы не
     * соприкасаются, между ними всегда виден просвет. */
    double pitch = (double)m->cols * APPS_CELL_W + APPS_PAGE_GAP;
    double off_from = -pitch * p * dir;
    double off_to = pitch * (1.0 - p) * dir;
    float op = (float)m->fade;
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        bool vis = cell->page == from || cell->page == to;
        wlr_scene_node_set_enabled(&cell->icon_node->node, vis);
        if (cell->label_node != NULL) {
            wlr_scene_node_set_enabled(&cell->label_node->node, vis);
        }
        if (!vis) {
            continue;
        }
        cell_shift(cell, cell->page == from ? off_from : off_to);
        wlr_scene_buffer_set_opacity(cell->icon_node, op);
        if (cell->label_node != NULL) {
            wlr_scene_buffer_set_opacity(cell->label_node, op);
        }
    }
}

static void menu_apply_page(struct mywm_apps_menu *m);

/* Фаза переворота текущей анимации. */
static void flip_apply(struct mywm_apps_menu *m, double p) {
    flip_render(m, m->flip_from, m->flip_to, m->flip_dir, p);
}

/* Завершение переворота: страница фиксируется, уходящие ячейки гасятся,
 * индикатор точек перерисовывается; при наличии очереди колеса —
 * следующий переворот. */
static void flip_finish(struct mywm_apps_menu *m) {
    m->flipping = false;
    m->page = m->flip_to;
    menu_apply_page(m);
    dots_update(m);
    if (m->flip_timer != NULL) {
        wl_event_source_timer_update(m->flip_timer, 0);
    }
    if (m->flip_pending >= 0 && m->flip_pending != m->page) {
        int target = m->flip_pending;
        m->flip_pending = -1;
        flip_start(m, target > m->page ? 1 : -1, target);
    } else {
        m->flip_pending = -1;
    }
}

/* Тик анимации перелистывания. */
static int flip_tick(void *data) {
    struct mywm_apps_menu *m = data;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    double dt = 1.0 / 60.0;
    if (m->flip_last > 0.0) {
        dt = now - m->flip_last;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.05) dt = 0.05;
    }
    m->flip_last = now;

    m->flip_t += dt / APPS_FLIP_S;
    if (m->flip_t >= 1.0) {
        flip_finish(m);
        return 0;
    }
    /* Smoothstep: мягкий разгон и мягкое торможение — как в macOS.
     * Свайп при этом рендерится линейно (без easing) в flip_render. */
    double p = m->flip_t * m->flip_t * (3.0 - 2.0 * m->flip_t);
    flip_apply(m, p);
    wl_event_source_timer_update(m->flip_timer, APPS_FLIP_MS);
    return 0;
}

/* Запуск анимации из from в to с начальной фазы t0 (0..1) — используется
 * колесом (t0=0) и доводкой свайпа (t0 = текущая фаза тяги). */
static void flip_launch(struct mywm_apps_menu *m, int from, int to,
                        int dir, double t0) {
    if (to < 0 || to >= m->pages || to == from) {
        return;
    }
    hover_hard_reset(m);
    m->flipping = true;
    m->flip_dir = dir;
    m->flip_from = from;
    m->flip_to = to;
    m->page = to;
    m->flip_t = t0;
    m->flip_last = 0.0;
    if (m->swiping) {
        m->swiping = false;
        if (m->swipe_timer != NULL) {
            wl_event_source_timer_update(m->swipe_timer, 0);
        }
    }
    double p0 = m->flip_t * m->flip_t * (3.0 - 2.0 * m->flip_t);
    flip_apply(m, p0);
    if (m->flip_timer != NULL) {
        wl_event_source_timer_update(m->flip_timer, 1);
    }
}

/* Старт переворота на страницу target (dir — направление жеста). */
static void flip_start(struct mywm_apps_menu *m, int dir, int target) {
    if (target < 0 || target >= m->pages || target == m->page) {
        return;
    }
    flip_launch(m, m->page, target, dir, 0.0);
}

/* --- Свайп двумя пальцами: страницы тянутся за пальцем --- */

/* Тик-детектор конца свайпа: пальцы молчат APPS_SWIPE_END_MS —
 * доводим до ближайшей страницы. */
static int swipe_end_tick(void *data) {
    struct mywm_apps_menu *m = data;
    if (!m->swiping) {
        return 0;
    }
    m->swiping = false;
    wl_event_source_timer_update(m->swipe_timer, 0);

    int dir = m->swipe_off >= 0 ? 1 : -1;
    double frac = fabs(m->swipe_off);
    int nb = m->swipe_base + dir;
    bool nb_ok = nb >= 0 && nb < m->pages;
    if (frac >= 0.5 && nb_ok) {
        /* Перешли половину — листаем, начиная с текущей фазы. */
        flip_launch(m, m->swipe_base, nb, dir, frac);
    } else if (frac > 0.02 && nb_ok) {
        /* Возврат на исходную: рендерим ту же пару в обратную
         * сторону, стартуя с зеркальной фазы. */
        flip_launch(m, nb, m->swipe_base, -dir, 1.0 - frac);
    } else {
        menu_apply_page(m);
    }
    return 0;
}

/* Обновление живого свайпа от очередного события оси пальцами. */
static void swipe_update(struct mywm_apps_menu *m, double delta) {
    if (m->flipping) {
        return; /* ждём окончания предыдущей анимации */
    }
    if (!m->swiping) {
        hover_hard_reset(m);
        m->swiping = true;
        m->swipe_base = m->page;
        m->swipe_off = 0.0;
    }
    double page_w = (double)m->cols * APPS_CELL_W;
    m->swipe_off += delta * APPS_SWIPE_GAIN / page_w;

    /* «Резина» за краями диапазона страниц. */
    double lo = -(double)m->swipe_base;
    double hi = (double)(m->pages - 1) - (double)m->swipe_base;
    if (m->swipe_off > hi) {
        m->swipe_off = hi + (m->swipe_off - hi) * APPS_SWIPE_RUBBER;
    } else if (m->swipe_off < lo) {
        m->swipe_off = lo + (m->swipe_off - lo) * APPS_SWIPE_RUBBER;
    }

    int dir = m->swipe_off >= 0 ? 1 : -1;
    int nb = m->swipe_base + dir;
    double frac = fabs(m->swipe_off);
    if ((nb < 0 || nb >= m->pages) && m->swipe_base >= 0 &&
            m->swipe_base < m->pages) {
        /* Соседа нет — тянем только базовую страницу лёгким сжатием. */
        flip_render(m, m->swipe_base, m->swipe_base, dir,
                    frac * APPS_SWIPE_RUBBER);
    } else {
        flip_render(m, m->swipe_base, nb, dir, frac);
    }
    wl_event_source_timer_update(m->swipe_timer, APPS_SWIPE_END_MS);
}

/* --- Точки-индикаторы страниц --- */

/* Пересоздаёт полосу точек под сеткой: текущая страница ярче.
 * node/tex уничтожаются и строятся заново — буфер у scene_buffer
 * подменить нельзя. */
static void dots_update(struct mywm_apps_menu *m) {
    if (m->dots != NULL) {
        wlr_scene_node_destroy(&m->dots->node);
        m->dots = NULL;
    }
    if (m->dots_tex != NULL) {
        wlr_buffer_unlock(&m->dots_tex->base);
        m->dots_tex = NULL;
    }
    m->dots_w = 0;
    m->dots_h = 0;
    if (m->pages <= 1) {
        return;
    }
    int w = m->pages * APPS_DOT_STEP;
    int h = (int)(APPS_DOT_R * 2.0) + 4;
    struct mywm_text_buf *tex = shell_blank_buf(w, h);
    if (tex == NULL) {
        return;
    }
    cairo_surface_t *ds = cairo_image_surface_create_for_data(
        tex->data, CAIRO_FORMAT_ARGB32, w, h, (int)tex->stride);
    cairo_t *dc = cairo_create(ds);
    for (int i = 0; i < m->pages; i++) {
        cairo_new_sub_path(dc);
        cairo_arc(dc, i * APPS_DOT_STEP + APPS_DOT_STEP / 2.0, h / 2.0,
                  APPS_DOT_R, 0, 2 * M_PI);
        cairo_set_source_rgba(dc, 1.0, 1.0, 1.0,
                              i == m->page ? 0.95 : 0.30);
        cairo_fill(dc);
    }
    cairo_destroy(dc);
    cairo_surface_destroy(ds);
    m->dots_tex = tex;
    m->dots = wlr_scene_buffer_create(m->tree, &tex->base);
    if (m->dots == NULL) {
        wlr_buffer_unlock(&tex->base);
        m->dots_tex = NULL;
        return;
    }
    m->dots_w = w;
    m->dots_h = h;
    m->dots_x = m->grid_x +
                (m->cols * APPS_CELL_W - w) / 2;
    m->dots_y = m->grid_top + m->rows_visible * m->cell_h + 10;
    wlr_scene_buffer_set_opacity(m->dots, (float)m->fade);
    wlr_scene_node_set_position(&m->dots->node, m->dots_x, m->dots_y);
}

/* Показывает только ячейки текущей страницы в базовой геометрии. */
static void menu_apply_page(struct mywm_apps_menu *m) {
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        bool vis = cell->page == m->page;
        wlr_scene_node_set_enabled(&cell->icon_node->node, vis);
        if (cell->label_node != NULL) {
            wlr_scene_node_set_enabled(&cell->label_node->node, vis);
        }
        if (vis) {
            cell_place_base(cell);
        }
    }
    if (m->hover_cell != NULL &&
            m->hover_cell->page != m->page) {
        m->hover_cell = NULL;
    }
}

/* Раскладка ячеек по страницам: позиции внутри страницы, номер
 * страницы, счётчики; затем показ текущей страницы. */
static void menu_relayout(struct mywm_apps_menu *m) {
    size_t i = 0;
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        int col = (int)(i % (size_t)m->cols);
        size_t row = i / (size_t)m->cols;
        cell->page = (int)(row / (size_t)m->rows_visible);
        int vis_row = (int)(row % (size_t)m->rows_visible);
        cell->lx = m->grid_x + col * APPS_CELL_W;
        cell->ly = m->grid_top + vis_row * m->cell_h;
        i++;
    }
    m->per_page = m->cols * m->rows_visible;
    m->pages = (int)((m->count + (size_t)m->per_page - 1) /
                     (size_t)m->per_page);
    if (m->pages < 1) {
        m->pages = 1;
    }
    if (m->page >= m->pages) {
        m->page = m->pages - 1;
    }
    if (m->page < 0) {
        m->page = 0;
    }
    menu_apply_page(m);
}

void apps_menu_init(struct mywm_server *server) {
    struct mywm_apps_menu *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return;
    }
    m->server = server;
    m->open = false;
    wl_list_init(&m->cells);
    m->tree = wlr_scene_tree_create(&server->scene->tree);
    m->bg = NULL;
    struct wl_event_loop *loop =
        wl_display_get_event_loop(server->wl_display);
    m->fade_timer = wl_event_loop_add_timer(loop, menu_fade_tick, m);
    m->hov_timer = wl_event_loop_add_timer(loop, hover_tick, m);
    m->flip_timer = wl_event_loop_add_timer(loop, flip_tick, m);
    m->swipe_timer = wl_event_loop_add_timer(loop, swipe_end_tick, m);
    m->flip_pending = -1;
    server->apps_menu = m;
    wlr_scene_node_set_enabled(&m->tree->node, false);
}

void apps_menu_toggle(struct mywm_server *server) {
    struct mywm_apps_menu *m = server->apps_menu;
    if (m == NULL) {
        return;
    }
    if (m->open) {
        m->open = false;
        /* Перелистывание посреди анимации — мгновенно доводим до
         * целевой страницы, чтобы фейд не затирал альфы полёта. */
        if (m->flipping) {
            m->flip_t = 1.0;
            flip_finish(m);
        }
        if (m->swiping) {
            m->swiping = false;
            if (m->swipe_timer != NULL) {
                wl_event_source_timer_update(m->swipe_timer, 0);
            }
            menu_apply_page(m);
        }
        if (m->fade_timer != NULL) {
            menu_fade_start(m, -1);
        } else {
            /* Таймер недоступен — мгновенное закрытие, иначе is_open
             * застрянет true и меню будет поглощать весь ввод. */
            wlr_scene_node_set_enabled(&m->tree->node, false);
            cells_destroy(m);
        }
        return;
    }

    /* Собираем список приложений заново при каждом открытии. Если
     * предыдущее закрытие ещё растворяется — сначала чистим его ячейки. */
    if (m->count > 0 || m->fade_dir == -1) {
        m->fade_dir = 0;
        cells_destroy(m);
    }
    static struct app_entry apps[APPS_MAX];
    size_t count = 0;
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    scan_dir("/usr/share/applications", apps, &count);
    scan_dir("/usr/local/share/applications", apps, &count);
    scan_dir("/var/lib/flatpak/exports/share/applications", apps, &count);
    if (home != NULL) {
        snprintf(path, sizeof(path), "%s/.local/share/applications", home);
        scan_dir(path, apps, &count);
        snprintf(path, sizeof(path), "%s/.flatpak/exports/share/applications",
                 home);
        scan_dir(path, apps, &count);
    }
    qsort(apps, count, sizeof(struct app_entry), cmp_entry);

    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    wlr_scene_node_set_position(&m->tree->node, box.x, box.y);
    /* Фон пересоздаём под текущий размер layout (текстура = фейдимый
     * scene_buffer; альфа запечена, opacity её масштабирует). */
    if (m->bg != NULL) {
        wlr_scene_node_destroy(&m->bg->node);
        m->bg = NULL;
    }
    float bg_color[4] = {0.04f, 0.04f, 0.06f, 0.92f};
    m->bg = icon_create_solid(&server->icon_mgr, m->tree,
                              box.width, box.height, bg_color);

    m->cols = (int)(box.width - 80) / APPS_CELL_W;
    if (m->cols > 8) {
        m->cols = 8;
    }
    if (m->cols < 3) {
        m->cols = 3;
    }
    m->cell_h = APPS_ICON + APPS_LABEL_PX + 28;
    m->grid_x = ((int)box.width - m->cols * APPS_CELL_W) / 2;
    m->grid_top = APPS_TOP_PAD;
    int avail_h = (int)box.height - APPS_TOP_PAD - APPS_BOTTOM_PAD;
    m->rows_visible = avail_h / m->cell_h;
    if (m->rows_visible < 1) {
        m->rows_visible = 1;
    }

    /* Пилюля-подсветка ховера: создаётся ДО ячеек — рендерится под
     * иконками, но над фоном. */
    m->pill_w = APPS_CELL_W - 18;
    m->pill_h = m->cell_h - 24;
    if (m->hover_bg != NULL) {
        wlr_scene_node_destroy(&m->hover_bg->node);
        m->hover_bg = NULL;
    }
    if (m->hover_tex != NULL) {
        wlr_buffer_unlock(&m->hover_tex->base);
        m->hover_tex = NULL;
    }
    m->hover_tex = shell_blank_buf(m->pill_w, m->pill_h);
    if (m->hover_tex != NULL) {
        cairo_surface_t *ps = cairo_image_surface_create_for_data(
            m->hover_tex->data, CAIRO_FORMAT_ARGB32, m->pill_w, m->pill_h,
            (int)m->hover_tex->stride);
        cairo_t *pc = cairo_create(ps);
        double pr = 14.0;
        double pw = m->pill_w, ph = m->pill_h;
        cairo_move_to(pc, pr, 0);
        cairo_line_to(pc, pw - pr, 0);
        cairo_arc(pc, pw - pr, pr, pr, -M_PI / 2, 0);
        cairo_line_to(pc, pw, ph - pr);
        cairo_arc(pc, pw - pr, ph - pr, pr, 0, M_PI / 2);
        cairo_line_to(pc, pr, ph);
        cairo_arc(pc, pr, ph - pr, pr, M_PI / 2, M_PI);
        cairo_line_to(pc, 0, pr);
        cairo_arc(pc, pr, pr, pr, M_PI, 3 * M_PI / 2);
        cairo_close_path(pc);
        cairo_set_source_rgba(pc, 1.0, 1.0, 1.0, APPS_PILL_ALPHA);
        cairo_fill(pc);
        cairo_destroy(pc);
        cairo_surface_destroy(ps);

        m->hover_bg = wlr_scene_buffer_create(m->tree, &m->hover_tex->base);
        if (m->hover_bg != NULL) {
            wlr_scene_node_set_enabled(&m->hover_bg->node, false);
        }
    }
    m->hover_cell = NULL;
    m->pill_a = 0.0f;

    for (size_t idx = 0; idx < count; idx++) {
        struct apps_cell *cell = calloc(1, sizeof(*cell));
        if (cell == NULL) {
            break;
        }
        cell->app = apps[idx];
        const char *icon_name =
            cell->app.icon[0] != '\0' ? cell->app.icon
                                      : "application-x-executable";
        cell->icon_node = icon_load_app(&server->icon_mgr, m->tree,
                                        icon_name, APPS_ICON);
        cell->label_buf = shell_label_buf(server, cell->app.name,
                                          APPS_LABEL_PX);
        if (cell->label_buf != NULL) {
            wlr_buffer_lock(&cell->label_buf->base);
            cell->label_node = wlr_scene_buffer_create(
                m->tree, &cell->label_buf->base);
        }
        wl_list_insert(m->cells.prev, &cell->link);
        m->count++;
    }

    m->page = 0;
    m->flip_pending = -1;
    m->open = true;
    wlr_scene_node_set_enabled(&m->tree->node, true);
    wlr_scene_node_raise_to_top(&m->tree->node);
    menu_relayout(m);
    dots_update(m);
    /* Курсор мог уже стоять над ячейкой — показываем ховер сразу. */
    apps_menu_motion(server, server->cursor->x, server->cursor->y);
    /* Анимация появления. */
    if (m->fade_dir == 0) {
        m->fade = 0.0;
        menu_apply_fade(m);
        menu_fade_start(m, 1);
    }
    wlr_log(WLR_INFO, "apps menu: opened with %zu applications", m->count);
}

bool apps_menu_is_open(const struct mywm_server *server) {
    /* Считаем меню открытым, пока идёт анимация закрытия: перехватываем
     * ввод, чтобы клики не «проваливались» сквозь растворяющийся оверлей. */
    struct mywm_apps_menu *m = server->apps_menu;
    return m != NULL && (m->open || m->fade_dir == -1);
}

/* Дерево меню в сцене (для session_lock.c: скрытие при блокировке). */
struct wlr_scene_tree *apps_menu_tree(struct mywm_server *server) {
    struct mywm_apps_menu *m = server->apps_menu;
    return m != NULL ? m->tree : NULL;
}

/* Прокрутка в меню: колесо мыши листает дискретно через накопитель
 * порога, свайп двумя пальцами (source == finger) тянет страницы
 * живьём с доводкой после остановки пальцев. */
void apps_menu_scroll(struct mywm_server *server, double delta,
                      uint32_t source) {
    struct mywm_apps_menu *m = server->apps_menu;
    if (m == NULL || !m->open || delta == 0.0) {
        return;
    }
    if (source == WL_POINTER_AXIS_SOURCE_FINGER) {
        swipe_update(m, delta);
        return;
    }
    /* Колесо: чтобы случайное касание или один «щелчок» не листали
     * страницу, дельты копятся в scroll_acc; пауза дольше
     * APPS_SCROLL_IDLE_S сбрасывает накопление. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec / 1e9;
    if (now - m->scroll_last > APPS_SCROLL_IDLE_S) {
        m->scroll_acc = 0.0;
    }
    m->scroll_last = now;
    m->scroll_acc += delta;

    if (fabs(m->scroll_acc) < APPS_SCROLL_THRESHOLD) {
        return;
    }
    int dir = m->scroll_acc > 0 ? 1 : -1;
    m->scroll_acc = 0.0;

    if (m->flipping || m->swiping) {
        /* Копим намерение от последнего запрошенного листания. */
        int base = m->flip_pending >= 0 ? m->flip_pending : m->page;
        int next = base + dir;
        if (next < 0) {
            next = 0;
        }
        if (next >= m->pages) {
            next = m->pages - 1;
        }
        m->flip_pending = next;
        return;
    }
    flip_start(m, dir, m->page + dir);
}

/* Движение курсора над открытым меню: выбор ячейки под курсором и
 * переключение ховера. Координаты — глобальные (layout), как в click. */
void apps_menu_motion(struct mywm_server *server, double lx, double ly) {
    struct mywm_apps_menu *m = server->apps_menu;
    if (m == NULL || !m->open || m->flipping || m->swiping) {
        return;
    }
    struct apps_cell *pick = NULL;
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        if (!cell->icon_node->node.enabled) {
            continue;
        }
        if (lx >= cell->lx && lx < cell->lx + APPS_CELL_W &&
                ly >= cell->ly && ly < cell->ly + m->cell_h - 16) {
            pick = cell;
            break;
        }
    }
    if (pick == m->hover_cell) {
        return;
    }
    if (m->hover_cell != NULL) {
        m->hover_cell->ht = 0.0f;
    }
    m->hover_cell = pick;
    if (pick != NULL) {
        pick->ht = 1.0f;
        hover_pill_place(m);
    }
    hover_kick(m);
}

bool apps_menu_click(struct mywm_server *server, double lx, double ly) {
    struct mywm_apps_menu *m = server->apps_menu;
    if (m == NULL || !m->open) {
        return false;
    }
    if (m->flipping || m->swiping) {
        return true; /* Во время переворота/свайпа клики игнорируем. */
    }
    /* Клик по точкам-индикаторам — прыжок на страницу. */
    if (m->pages > 1 && m->dots_w > 0 &&
            ly >= m->dots_y - 6 && ly < m->dots_y + m->dots_h + 6 &&
            lx >= m->dots_x - 6 && lx < m->dots_x + m->dots_w + 6) {
        int idx = (int)((lx - m->dots_x) / APPS_DOT_STEP);
        if (idx < 0) {
            idx = 0;
        }
        if (idx >= m->pages) {
            idx = m->pages - 1;
        }
        if (idx != m->page) {
            flip_start(m, idx > m->page ? 1 : -1, idx);
        }
        return true;
    }
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        if (!cell->icon_node->node.enabled) {
            continue;
        }
        if (lx >= cell->lx && lx < cell->lx + APPS_CELL_W &&
                ly >= cell->ly && ly < cell->ly + m->cell_h - 16) {
            wlr_log(WLR_INFO, "apps menu: launching '%s'", cell->app.exec);
            mywm_spawn(server, cell->app.exec);
            apps_menu_toggle(server); /* закрыть */
            return true;
        }
    }
    /* Клик мимо иконки — закрыть меню. */
    apps_menu_toggle(server);
    return true;
}
