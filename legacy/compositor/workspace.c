/*
 * workspace.c — рабочие столы (Spaces) в стиле macOS.
 *
 * Модель: каждому столу соответствует scene-дерево — ребёнок view_tree.
 * Дерево нового окна — дерево активного стола; видимость стола — это
 * enabled его дерева (как у свёрнутых окон: флаги детей не трогаются).
 * Переключение анимируется слайдом обоих деревьев по X: контейнер
 * сдвигается, абсолютные координаты окон не меняются.
 *
 * Столы создаются лениво при первом обращении (@ws 5 создаст 0..4).
 */
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

/* Длительность слайда, мс. */
#define WS_ANIM_MS 240
/* Период тика анимации, мс. */
#define WS_TICK_MS 16

/* --- Файловый IPC для оболочки (QuickShell TopBar) --- */
/* Состояние: $XDG_RUNTIME_DIR/de/workspaces — строка "current count"
 * (1-based). Команды: $XDG_RUNTIME_DIR/de/ws-cmd (FIFO) — "next",
 * "prev" или номер стола. Простой и надежный канал без новых
 * wayland-протоколов; qs читает файл через FileView. */

static void ws_ipc_publish(struct mywm_server *server) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == NULL || runtime[0] == '\0') {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/de", runtime);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/de/workspaces", runtime);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "%d %d\n", workspace_current_index(server) + 1,
            workspace_count(server));
    fclose(f);
}

/* Обработчик команд из FIFO (неблокирующее чтение).
 * Формат строк: "next" | "prev" | "N" (стол с 1) | "move N" (окно на
 * стол N) | "spawn <команда>". Пачки строк разбираются полностью. */
static int ws_cmd_read(int fd, uint32_t mask, void *data) {
    struct mywm_server *server = data;
    (void)mask;
    char buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            break; /* EAGAIN или EOF(O_RDWR не даёт EOF) */
        }
        buf[n] = '\0';
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line != NULL;
                line = strtok_r(NULL, "\n", &save)) {
            if (strcmp(line, "next") == 0) {
                workspace_switch_next(server);
                continue;
            }
            if (strcmp(line, "prev") == 0) {
                workspace_switch_prev(server);
                continue;
            }
            if (strncmp(line, "move ", 5) == 0) {
                char *end = NULL;
                long idx = strtol(line + 5, &end, 10);
                if (end != line + 5 && idx >= 1 && idx <= WS_MAX &&
                        server->focused_view != NULL) {
                    workspace_move_view(server->focused_view, (int)idx - 1);
                }
                continue;
            }
            if (strncmp(line, "spawn ", 6) == 0) {
                mywm_spawn(server, line + 6);
                continue;
            }
            char *end = NULL;
            long idx = strtol(line, &end, 10);
            if (end != line && idx >= 1 && idx <= WS_MAX) {
                workspace_switch(server, (int)idx - 1);
            }
        }
    }
    return 0;
}

static void ws_ipc_init(struct mywm_server *server) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == NULL || runtime[0] == '\0') {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/de", runtime);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/de/ws-cmd", runtime);
    if (mkfifo(path, 0666) != 0 && errno != EEXIST) {
        wlr_log(WLR_ERROR, "workspaces: mkfifo failed: %s", strerror(errno));
        return;
    }
    /* O_RDWR: писатель может не открывать наш конец; без EOF-спама. */
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        wlr_log(WLR_ERROR, "workspaces: fifo open failed: %s", strerror(errno));
        return;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE, ws_cmd_read, server);
}

int workspace_current_index(const struct mywm_server *server) {
    return server->ws.current != NULL ? server->ws.current->index : 0;
}

int workspace_count(const struct mywm_server *server) {
    int n = 0;
    struct mywm_workspace *ws;
    wl_list_for_each(ws, &server->ws.list, link) {
        n++;
    }
    return n;
}

bool workspace_view_visible(const struct mywm_view *view) {
    return view->ws != NULL && view->ws == view->server->ws.current;
}

/* Найти стол по индексу; при отсутствии создать (дерево скрыто). */
static struct mywm_workspace *workspace_get(struct mywm_server *server,
                                            int index) {
    if (index < 0 || index >= WS_MAX) {
        return NULL;
    }
    struct mywm_workspace *it;
    wl_list_for_each(it, &server->ws.list, link) {
        if (it->index == index) {
            return it;
        }
        if (it->index > index) {
            break; /* вставка перед it */
        }
    }
    struct mywm_workspace *ws = calloc(1, sizeof(*ws));
    if (ws == NULL) {
        return NULL;
    }
    ws->server = server;
    ws->index = index;
    ws->tree = wlr_scene_tree_create(server->view_tree);
    if (ws->tree == NULL) {
        free(ws);
        return NULL;
    }
    /* Новый стол скрыт, пока на него не переключились. */
    wlr_scene_node_set_enabled(&ws->tree->node, false);
    if (&it->link == &server->ws.list) {
        wl_list_insert(server->ws.list.prev, &ws->link); /* в хвост */
    } else {
        wl_list_insert(it->link.prev, &ws->link);        /* перед it */
    }
    return ws;
}

struct wlr_scene_tree *workspace_active_tree(struct mywm_server *server) {
    return server->ws.current != NULL ? server->ws.current->tree
                                      : server->view_tree;
}

/* Мгновенно завершить идущий слайд (снап в конечное состояние). */
static void ws_anim_finish(struct mywm_server *server) {
    struct mywm_workspaces *ws = &server->ws;
    if (ws->anim_timer != NULL && ws->dir != 0) {
        wl_event_source_timer_update(ws->anim_timer, 0);
    }
    if (ws->from != NULL) {
        wlr_scene_node_set_position(&ws->from->tree->node, 0, 0);
        wlr_scene_node_set_enabled(&ws->from->tree->node,
                                   ws->from == ws->current);
    }
    if (ws->to != NULL) {
        wlr_scene_node_set_position(&ws->to->tree->node, 0, 0);
        wlr_scene_node_set_enabled(&ws->to->tree->node,
                                   ws->to == ws->current);
    }
    ws->from = NULL;
    ws->to = NULL;
    ws->dir = 0;
}

/* Тик слайда: ease-out cubic по прогрессу, сдвиг обоих деревьев. */
static int ws_anim_tick(void *data) {
    struct mywm_server *server = data;
    struct mywm_workspaces *ws = &server->ws;
    if (ws->dir == 0 || ws->from == NULL || ws->to == NULL) {
        return 0;
    }
    ws->progress += (double)WS_TICK_MS / WS_ANIM_MS;
    if (ws->progress >= 1.0) {
        ws_anim_finish(server);
        return 0;
    }
    double ease = 1.0 - (1.0 - ws->progress) * (1.0 - ws->progress) *
                            (1.0 - ws->progress);
    int off_from = (int)(-ws->dir * ws->width * ease);
    int off_to = (int)(ws->dir * ws->width * (1.0 - ease));
    wlr_scene_node_set_position(&ws->from->tree->node, off_from, 0);
    wlr_scene_node_set_position(&ws->to->tree->node, off_to, 0);
    wl_event_source_timer_update(ws->anim_timer, WS_TICK_MS);
    return 0;
}

void workspace_switch(struct mywm_server *server, int index) {
    struct mywm_workspaces *ws = &server->ws;
    if (index < 0) {
        index = 0;
    }
    if (index >= WS_MAX) {
        index = WS_MAX - 1;
    }
    if (ws->current != NULL && index == ws->current->index) {
        return;
    }
    struct mywm_workspace *target = workspace_get(server, index);
    if (target == NULL || target == ws->current) {
        return;
    }

    int dir = index > workspace_current_index(server) ? 1 : -1;
    wlr_log(WLR_INFO, "workspace switch %d -> %d",
            workspace_current_index(server), index);

    /* Скрыть окна старого стола от фокуса: сбросим фокус, если он был
     * на окне уходящего стола (вернём после переключения на новый). */
    struct mywm_view *fv = server->focused_view;

    ws_anim_finish(server);

    /* Слайд: ширина всего layout (обои не двигаются — они ниже). */
    struct wlr_box box;
    wlr_output_layout_get_box(server->output_layout, NULL, &box);
    int width = box.width > 0 ? box.width : 1280;

    ws->from = ws->current;
    ws->to = target;
    ws->dir = dir;
    ws->width = width;
    ws->progress = 0.0;
    ws->current = target;

    wlr_scene_node_set_enabled(&ws->to->tree->node, true);
    wlr_scene_node_set_position(&ws->to->tree->node, dir * width, 0);
    if (ws->from != NULL && ws->from != ws->to) {
        wlr_scene_node_set_enabled(&ws->from->tree->node, true);
        wlr_scene_node_set_position(&ws->from->tree->node, 0, 0);
    }
    if (ws->anim_timer != NULL) {
        wl_event_source_timer_update(ws->anim_timer, WS_TICK_MS);
    }
    ws_ipc_publish(server);

    /* Фокус: первое видимое окно нового стола (fv уходит со старым). */
    struct mywm_view *it;
    wl_list_for_each(it, &server->views, link) {
        if (it->mapped && workspace_view_visible(it)) {
            focus_view(server, it, it->xdg_toplevel->base->surface);
            return;
        }
    }
    /* Новый стол пуст: окно с уходящего стола теряет фокус. */
    if (fv != NULL && fv->mapped) {
        wlr_seat_keyboard_clear_focus(server->seat);
    }
}

void workspace_switch_next(struct mywm_server *server) {
    int next = workspace_current_index(server) + 1;
    if (next >= WS_MAX) {
        next = WS_MAX - 1;
    }
    workspace_switch(server, next);
}

void workspace_switch_prev(struct mywm_server *server) {
    int prev = workspace_current_index(server) - 1;
    if (prev < 0) {
        prev = 0;
    }
    workspace_switch(server, prev);
}

static void workspace_move_to(struct mywm_view *view,
                              struct mywm_workspace *target) {
    if (view == NULL || target == NULL || view->ws == target) {
        return;
    }
    view->ws = target;
    /* Переподвешиваем контейнер декораций в дерево целевого стола;
     * абсолютная позиция окна сохраняется. */
    wlr_scene_node_reparent(&view->deco_tree->node, target->tree);
    wlr_scene_node_raise_to_top(&view->deco_tree->node);
}

void workspace_move_view(struct mywm_view *view, int index) {
    if (view == NULL) {
        return;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= WS_MAX) {
        index = WS_MAX - 1;
    }
    struct mywm_workspace *target =
        workspace_get(view->server, index);
    if (target == NULL) {
        return;
    }
    workspace_move_to(view, target);
    /* macOS: окно «уезжает» вместе с пользователем. */
    workspace_switch(view->server, index);
}

void workspace_move_view_next(struct mywm_view *view) {
    if (view == NULL || view->ws == NULL) {
        return;
    }
    workspace_move_view(view, view->ws->index + 1);
}

void workspace_move_view_prev(struct mywm_view *view) {
    if (view == NULL || view->ws == NULL || view->ws->index == 0) {
        return;
    }
    workspace_move_view(view, view->ws->index - 1);
}

void workspaces_init(struct mywm_server *server) {
    wl_list_init(&server->ws.list);
    server->ws.current = NULL;
    server->ws.anim_timer = NULL;
    server->ws.dir = 0;
    server->ws.from = NULL;
    server->ws.to = NULL;

    struct mywm_workspace *first = workspace_get(server, 0);
    if (first == NULL) {
        wlr_log(WLR_ERROR, "workspaces: failed to create initial workspace");
        return;
    }
    first->index = 0;
    server->ws.current = first;
    wlr_scene_node_set_enabled(&first->tree->node, true);

    struct wl_event_loop *loop =
        wl_display_get_event_loop(server->wl_display);
    server->ws.anim_timer =
        wl_event_loop_add_timer(loop, ws_anim_tick, server);

    /* IPC для оболочки: состояние + команды. */
    ws_ipc_init(server);
    ws_ipc_publish(server);
}
