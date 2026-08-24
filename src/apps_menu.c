#include "server.h"
#include <cairo.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
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
 * XDG-каталогов при каждом открытии. Колесо — прокрутка страниц, клик по
 * иконке — запуск, клик мимо / Esc / повторный клик по пину — закрытие.
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
};

struct mywm_apps_menu {
    struct mywm_server *server;
    struct wlr_scene_tree *tree;
    /* Фон — текстура (не scene_rect): у буфера есть opacity для фейда. */
    struct wlr_scene_buffer *bg;
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
    m->count = 0;
    m->scroll = 0;
}

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

/* Перескладка видимых строк под текущий scroll. */
static void menu_relayout(struct mywm_apps_menu *m) {    int i = 0;
    struct apps_cell *cell;
    wl_list_for_each(cell, &m->cells, link) {
        int col = i % m->cols;
        int row = i / m->cols;
        int vis_row = row - m->scroll;
        bool visible = vis_row >= 0 && vis_row < m->rows_visible;
        wlr_scene_node_set_enabled(&cell->icon_node->node, visible);
        if (cell->label_node != NULL) {
            wlr_scene_node_set_enabled(&cell->label_node->node, visible);
        }
        cell->lx = m->grid_x + col * APPS_CELL_W;
        cell->ly = m->grid_top + vis_row * m->cell_h;
        wlr_scene_node_set_position(&cell->icon_node->node,
                                    cell->lx +
                                        (APPS_CELL_W - APPS_ICON) / 2,
                                    cell->ly);
        if (cell->label_node != NULL && cell->label_buf != NULL) {
            wlr_scene_node_set_position(
                &cell->label_node->node,
                cell->lx + (APPS_CELL_W -
                            cell->label_buf->base.width) / 2,
                cell->ly + APPS_ICON + 4);
        }
        i++;
    }
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
        menu_fade_start(m, -1);
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

    m->scroll = 0;
    m->open = true;
    wlr_scene_node_set_enabled(&m->tree->node, true);
    wlr_scene_node_raise_to_top(&m->tree->node);
    menu_relayout(m);
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

void apps_menu_scroll(struct mywm_server *server, double delta) {
    struct mywm_apps_menu *m = server->apps_menu;
    if (m == NULL || !m->open || delta == 0.0) {
        return;
    }
    size_t total_rows = (m->count + (size_t)m->cols - 1) / (size_t)m->cols;
    int max_scroll = (int)total_rows - m->rows_visible;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    int step = delta > 0 ? 1 : -1;
    int next = m->scroll + step;
    if (next < 0) {
        next = 0;
    }
    if (next > max_scroll) {
        next = max_scroll;
    }
    if (next != m->scroll) {
        m->scroll = next;
        menu_relayout(m);
    }
}

bool apps_menu_click(struct mywm_server *server, double lx, double ly) {
    struct mywm_apps_menu *m = server->apps_menu;
    if (m == NULL || !m->open) {
        return false;
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
