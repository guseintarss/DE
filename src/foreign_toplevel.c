/*
 * foreign_toplevel.c — wlr-foreign-toplevel-management-v1.
 *
 * Публикует каждое xdg-окно как toplevel-хэндл: внешние оболочки
 * (waybar wlr/taskbar, AGSv2, QuickShell) получают список окон,
 * заголовки/app_id/состояния и могут управлять ими (activate/close/
 * minimize/maximize).
 */
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

struct mywm_ftl_toplevel {
    struct wl_list link;            /* server->ftl_toplevels */
    struct mywm_view *view;
    struct wlr_foreign_toplevel_handle_v1 *handle;
    struct wl_listener request_activate;
    struct wl_listener request_close;
    struct wl_listener request_minimize;
    struct wl_listener request_maximize;
    struct wl_listener handle_destroy;
};

static void ftl_refresh_state(struct mywm_ftl_toplevel *t) {
    if (t == NULL || t->handle == NULL) {
        return;
    }
    /* В wlroots-0.20 нет set_state — состояния публикуются
     * гранулярными сеттерами. */
    struct mywm_view *view = t->view;
    bool maximized = view != NULL && view->maximized;
    bool minimized = view != NULL && (view->minimized || !view->mapped);
    /* ACTIVATED: окно сейчас в фокусе и не свёрнуто. */
    bool activated = view != NULL && view->server->focused_view == view &&
                     view->mapped && !view->minimized;
    wlr_foreign_toplevel_handle_v1_set_maximized(t->handle, maximized);
    wlr_foreign_toplevel_handle_v1_set_minimized(t->handle, minimized);
    wlr_foreign_toplevel_handle_v1_set_activated(t->handle, activated);
}

static struct mywm_ftl_toplevel *ftl_from_view(struct mywm_view *view) {
    struct mywm_server *server = view->server;
    struct mywm_ftl_toplevel *t;
    wl_list_for_each(t, &server->ftl_toplevels, link) {
        if (t->view == view) {
            return t;
        }
    }
    return NULL;
}

/* --- Запросы от клиентов (таскбар кликнул по иконке и т.п.) --- */

static void ftl_request_activate_handler(struct wl_listener *listener,
                                         void *data) {
    struct mywm_ftl_toplevel *t =
        wl_container_of(listener, t, request_activate);
    (void)data;
    struct mywm_view *view = t->view;
    if (view != NULL && view->mapped) {
        focus_view(view->server, view, view->xdg_toplevel->base->surface);
    }
}

static void ftl_request_close_handler(struct wl_listener *listener,
                                      void *data) {
    struct mywm_ftl_toplevel *t =
        wl_container_of(listener, t, request_close);
    (void)data;
    if (t->view != NULL) {
        close_view(t->view);
    }
}

static void ftl_request_minimize_handler(struct wl_listener *listener,
                                         void *data) {
    struct mywm_ftl_toplevel *t =
        wl_container_of(listener, t, request_minimize);
    struct wlr_foreign_toplevel_handle_v1_minimized_event *event = data;
    struct mywm_view *view = t->view;
    if (view == NULL || event == NULL) {
        return;
    }
    if (event->minimized) {
        minimize_view(view);
    } else if (view->mapped) {
        /* Разворачивание = фокус (focus_view сам запускает genie-out). */
        focus_view(view->server, view, view->xdg_toplevel->base->surface);
    }
}

static void ftl_request_maximize_handler(struct wl_listener *listener,
                                         void *data) {
    struct mywm_ftl_toplevel *t =
        wl_container_of(listener, t, request_maximize);
    struct wlr_foreign_toplevel_handle_v1_maximized_event *event = data;
    struct mywm_view *view = t->view;
    if (view == NULL || event == NULL || !view->mapped ||
            view->minimized) {
        return;
    }
    if (view->maximized != event->maximized) {
        maximize_view(view);   /* toggle: maximize/restore */
    }
}

static void ftl_handle_destroy_handler(struct wl_listener *listener,
                                       void *data) {
    struct mywm_ftl_toplevel *t =
        wl_container_of(listener, t, handle_destroy);
    (void)data;
    wl_list_remove(&t->link);
    wl_list_remove(&t->request_activate.link);
    wl_list_remove(&t->request_close.link);
    wl_list_remove(&t->request_minimize.link);
    wl_list_remove(&t->request_maximize.link);
    wl_list_remove(&t->handle_destroy.link);
    free(t);
}

/* --- Публичные хуки (вызывается из xdg_shell.c) --- */

void foreign_toplevel_map(struct mywm_view *view) {
    struct mywm_server *server = view->server;
    if (server->ftl_manager == NULL) {
        return;
    }
    if (ftl_from_view(view) != NULL) {
        ftl_refresh_state(ftl_from_view(view));
        return;
    }
    struct mywm_ftl_toplevel *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return;
    }
    t->view = view;
    t->handle = wlr_foreign_toplevel_handle_v1_create(server->ftl_manager);
    if (t->handle == NULL) {
        free(t);
        return;
    }

    const char *title = view->xdg_toplevel->title;
    if (title != NULL && title[0] != '\0') {
        wlr_foreign_toplevel_handle_v1_set_title(t->handle, title);
    }
    const char *app_id = view->xdg_toplevel->app_id;
    if (app_id != NULL && app_id[0] != '\0') {
        wlr_foreign_toplevel_handle_v1_set_app_id(t->handle, app_id);
    }

    t->request_activate.notify = ftl_request_activate_handler;
    wl_signal_add(&t->handle->events.request_activate,
                  &t->request_activate);
    t->request_close.notify = ftl_request_close_handler;
    wl_signal_add(&t->handle->events.request_close, &t->request_close);
    t->request_minimize.notify = ftl_request_minimize_handler;
    wl_signal_add(&t->handle->events.request_minimize,
                  &t->request_minimize);
    t->request_maximize.notify = ftl_request_maximize_handler;
    wl_signal_add(&t->handle->events.request_maximize,
                  &t->request_maximize);
    t->handle_destroy.notify = ftl_handle_destroy_handler;
    wl_signal_add(&t->handle->events.destroy, &t->handle_destroy);

    wl_list_insert(&server->ftl_toplevels, &t->link);
    ftl_refresh_state(t);
}

/*
 * unmap окна. Хэндл уничтожаем: протокол не позволяет «спрятать» окно,
 * а повторный map создаст новый хэндл (таскбары так умеют).
 */
void foreign_toplevel_unmap(struct mywm_view *view) {
    struct mywm_ftl_toplevel *t = ftl_from_view(view);
    if (t != NULL && t->handle != NULL) {
        wlr_foreign_toplevel_handle_v1_destroy(t->handle);
        /* ftl_handle_destroy_handler освободит структуру. */
    }
}

void foreign_toplevel_title(struct mywm_view *view) {
    struct mywm_ftl_toplevel *t = ftl_from_view(view);
    if (t == NULL || t->handle == NULL) {
        return;
    }
    const char *title = view->xdg_toplevel->title;
    if (title != NULL && title[0] != '\0') {
        wlr_foreign_toplevel_handle_v1_set_title(t->handle, title);
    }
}

void foreign_toplevel_state(struct mywm_view *view) {
    ftl_refresh_state(ftl_from_view(view));
}

void foreign_toplevel_init(struct mywm_server *server) {
    wl_list_init(&server->ftl_toplevels);
    server->ftl_manager =
        wlr_foreign_toplevel_manager_v1_create(server->wl_display);
    if (server->ftl_manager == NULL) {
        wlr_log(WLR_ERROR, "Failed to create foreign-toplevel-manager");
    }
}
