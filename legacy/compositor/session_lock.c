/*
 * session_lock.c — безопасность и энергосбережение сессии.
 *
 * 1. ext-session-lock-v1: полноценная блокировка экрана. Клиент
 *    (swaylock и т.п.) создаёт lock-объект и поверхность на каждый
 *    выход; после первого commit мы отправляем locked, скрываем всё
 *    содержимое сцены (окна, слои, обои, встроенная оболочка) и
 *    показываем только поверхности блокировки. Клавиатура целиком
 *    уходит клиенту лок-скрина: биндинги и клики по оболочке не
 *    работают до unlock.
 *
 * 2. ext-idle-notify-v1: клиенты (swayidle) получают события простоя.
 *    Композитор кормит нотификатор активностью из обработчиков ввода
 *    (см. idle_notify_activity).
 *
 * 3. wlr-idle-inhibit: приложения (видеоплеер) запрещают простой.
 *    Упрощение: ингибирование считается активным, пока жив хоть один
 *    inhibitor, без проверки видимости его поверхности.
 */
#include "server.h"

#include <stdlib.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

/* Поверхность блокировки на одном выходе. */
struct mywm_lock_surface {
    struct wl_list link;            /* mywm_session_lock.surfaces */
    struct mywm_session_lock *lock;
    struct wlr_session_lock_surface_v1 *surface;
    struct wlr_scene_tree *tree;    /* ребёнок mgr->tree */
    bool committed;                 // пришёл буфер — можно локать
    struct wl_listener commit;
    struct wl_listener destroy;
};

/* Один lock-клиент (сеанс блокировки). */
struct mywm_session_lock {
    struct wl_list link;            /* mgr->locks */
    struct mywm_lock_manager *mgr;
    struct wlr_session_lock_v1 *lock;
    struct wl_list surfaces;        /* mywm_lock_surface::link */
    struct wl_listener new_surface;
    struct wl_listener unlock;
    struct wl_listener destroy;
};

struct mywm_lock_manager {
    struct mywm_server *server;
    struct wlr_session_lock_manager_v1 *mgr;
    struct wlr_session_lock_v1 *active;  // текущий сеанс или NULL
    bool locked;                         // locked отправлен, сцена скрыта
    struct wlr_scene_tree *tree;         // корень поверхностей блокировки
    struct wl_list locks;                // mywm_session_lock::link
    struct wl_listener new_lock;

    /* idle-inhibit: число живых inhibitor-объектов. */
    struct wl_listener new_inhibitor;
    int inhibitors;
};

bool session_lock_active(const struct mywm_server *server) {
    return server->lock != NULL && server->lock->locked;
}

void idle_notify_activity(struct mywm_server *server) {
    if (server->idle_notifier != NULL && server->seat != NULL) {
        wlr_idle_notifier_v1_notify_activity(server->idle_notifier,
                                             server->seat);
    }
}

/* --- Скрытие/показ содержимого сцены при блокировке --- */

static void scene_content_set_enabled(struct mywm_server *server,
                                      bool enabled) {
    wlr_scene_node_set_enabled(&server->view_tree->node, enabled);
    for (int i = 0; i < SHELL_LAYER_COUNT; i++) {
        wlr_scene_node_set_enabled(&server->layer_trees[i]->node, enabled);
    }
    /* Встроенная оболочка (может отсутствовать при [shell] builtin=false). */
    if (server->bar.tree != NULL) {
        wlr_scene_node_set_enabled(&server->bar.tree->node, enabled);
    }
    if (server->dock.tree != NULL) {
        wlr_scene_node_set_enabled(&server->dock.tree->node, enabled);
    }
    if (server->apps_menu != NULL) {
        struct wlr_scene_tree *menu = apps_menu_tree(server);
        if (menu != NULL) {
            wlr_scene_node_set_enabled(&menu->node, enabled);
        }
    }
    /* Обои — отдельное дерево на каждый выход. */
    struct mywm_output *out;
    wl_list_for_each(out, &server->outputs, link) {
        if (out->bg_tree != NULL) {
            wlr_scene_node_set_enabled(&out->bg_tree->node, enabled);
        }
    }
}

/* Переход в заблокированное состояние: после первого commit любой
 * lock-поверхности (клиент готов рисовать). */
static void lock_apply(struct mywm_lock_manager *m) {
    if (m->locked || m->active == NULL) {
        return;
    }
    m->locked = true;
    wlr_session_lock_v1_send_locked(m->active);

    scene_content_set_enabled(m->server, false);
    wlr_scene_node_raise_to_top(&m->tree->node);
    wlr_scene_node_set_enabled(&m->tree->node, true);
    wlr_log(WLR_INFO, "session locked");

    /* Клавиатура полностью уходит клиенту блокировки (требование
     * протокола): биндинги композитора не срабатывают — guard в
     * keyboard.c. Enter шлём только при наличии клавиатуры на seat
     * (headless без устройств — keymap NULL, wlr упадёт внутри). */
    struct mywm_lock_surface *ls = NULL;
    if (!wl_list_empty(&m->active->surfaces)) {
        ls = wl_container_of(m->active->surfaces.next, ls, link);
    }
    if (ls != NULL && wlr_seat_get_keyboard(m->server->seat) != NULL) {
        wlr_seat_keyboard_notify_enter(m->server->seat,
                                       ls->surface->surface, NULL, 0, NULL);
    }
}

static void lock_apply_unlock(struct mywm_lock_manager *m) {
    if (!m->locked) {
        return;
    }
    m->locked = false;
    wlr_scene_node_set_enabled(&m->tree->node, false);
    scene_content_set_enabled(m->server, true);
    wlr_log(WLR_INFO, "session unlocked");

    /* Вернуть клавиатуру сфокусированному окну (если оно живо). */
    struct mywm_server *s = m->server;
    if (s->focused_view != NULL && s->focused_view->mapped) {
        focus_view(s, s->focused_view,
                   s->focused_view->xdg_toplevel->base->surface);
    } else {
        wlr_seat_keyboard_clear_focus(s->seat);
    }
}

/* --- Поверхность блокировки --- */

static void lock_surface_destroy(struct wl_listener *listener, void *data) {
    struct mywm_lock_surface *ls =
        wl_container_of(listener, ls, destroy);
    (void)data;
    wl_list_remove(&ls->link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->destroy.link);
    if (ls->tree != NULL) {
        wlr_scene_node_destroy(&ls->tree->node);
    }
    free(ls);
}

static void lock_surface_commit(struct wl_listener *listener, void *data) {
    struct mywm_lock_surface *ls = wl_container_of(listener, ls, commit);
    (void)data;
    if (ls->committed || ls->surface->surface->buffer == NULL) {
        return;
    }
    ls->committed = true;
    /* Первый буфер от клиента — можно объявлять сессию заблокированной. */
    lock_apply(ls->lock->mgr);
}

static void lock_handle_new_surface(struct wl_listener *listener,
                                    void *data) {
    struct mywm_session_lock *l = wl_container_of(listener, l, new_surface);
    struct wlr_session_lock_surface_v1 *surface = data;
    struct mywm_lock_manager *m = l->mgr;

    struct mywm_lock_surface *ls = calloc(1, sizeof(*ls));
    if (ls == NULL) {
        return;
    }
    ls->lock = l;
    ls->surface = surface;

    /* Сразу конфигурируем размером выхода; клиент пришлёт буфер. */
    wlr_session_lock_surface_v1_configure(surface,
                                          surface->output->width,
                                          surface->output->height);

    ls->tree = wlr_scene_tree_create(m->tree);
    if (ls->tree == NULL) {
        free(ls);
        return;
    }
    wlr_scene_surface_create(ls->tree, surface->surface);
    /* Позиция выхода в координатах layout (мульти-монитор). */
    double lx = 0, ly = 0;
    wlr_output_layout_output_coords(m->server->output_layout,
                                    surface->output, &lx, &ly);
    wlr_scene_node_set_position(&ls->tree->node, (int)lx, (int)ly);

    ls->commit.notify = lock_surface_commit;
    wl_signal_add(&surface->surface->events.commit, &ls->commit);
    ls->destroy.notify = lock_surface_destroy;
    wl_signal_add(&surface->surface->events.destroy, &ls->destroy);

    wl_list_insert(l->surfaces.prev, &ls->link);
}

/* --- Сеанс блокировки --- */

static void lock_destroy(struct wl_listener *listener, void *data) {
    struct mywm_session_lock *l = wl_container_of(listener, l, destroy);
    (void)data;
    lock_apply_unlock(l->mgr);
    wl_list_remove(&l->link);
    wl_list_remove(&l->new_surface.link);
    wl_list_remove(&l->unlock.link);
    wl_list_remove(&l->destroy.link);
    /* Оставшиеся поверхности уничтожатся своими destroy-обработчиками
     * (wlr роняет wlr_surface до lock-ресурса). */
    if (l->mgr->active == l->lock) {
        l->mgr->active = NULL;
    }
    free(l);
}

static void lock_handle_unlock(struct wl_listener *listener, void *data) {
    struct mywm_session_lock *l = wl_container_of(listener, l, unlock);
    (void)data;
    lock_apply_unlock(l->mgr);
}

static void lock_handle_new_lock(struct wl_listener *listener, void *data) {
    struct mywm_lock_manager *m = wl_container_of(listener, m, new_lock);
    struct wlr_session_lock_v1 *wlr_lock = data;

    /* Второй одновременный лок-клиент не допускается. */
    if (m->active != NULL) {
        wlr_session_lock_v1_destroy(wlr_lock);
        return;
    }

    struct mywm_session_lock *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        wlr_session_lock_v1_destroy(wlr_lock);
        return;
    }
    l->mgr = m;
    l->lock = wlr_lock;
    wl_list_init(&l->surfaces);

    l->new_surface.notify = lock_handle_new_surface;
    wl_signal_add(&wlr_lock->events.new_surface, &l->new_surface);
    l->unlock.notify = lock_handle_unlock;
    wl_signal_add(&wlr_lock->events.unlock, &l->unlock);
    l->destroy.notify = lock_destroy;
    wl_signal_add(&wlr_lock->events.destroy, &l->destroy);

    wl_list_insert(m->locks.prev, &l->link);
    m->active = wlr_lock;
    wlr_log(WLR_INFO, "session lock client connected");
}

/* --- Idle inhibit --- */

/* Обёртка: на каждый inhibitor вешаем одноразовый destroy-хук. */
struct mywm_inhibitor {
    struct wlr_idle_inhibitor_v1 *inhibitor;
    struct mywm_lock_manager *mgr;
    struct wl_listener destroy;
};

static void inhibitor_handle_destroy(struct wl_listener *listener,
                                     void *data) {
    struct mywm_inhibitor *inh = wl_container_of(listener, inh, destroy);
    (void)data;
    struct mywm_lock_manager *m = inh->mgr;
    wl_list_remove(&inh->destroy.link);
    if (m->inhibitors > 0) {
        m->inhibitors--;
    }
    wlr_idle_notifier_v1_set_inhibited(m->server->idle_notifier,
                                       m->inhibitors > 0);
    free(inh);
}

static void lock_handle_new_inhibitor(struct wl_listener *listener,
                                      void *data) {
    struct mywm_lock_manager *m = wl_container_of(listener, m, new_inhibitor);
    struct wlr_idle_inhibitor_v1 *wlr_inh = data;

    struct mywm_inhibitor *inh = calloc(1, sizeof(*inh));
    if (inh == NULL) {
        return;
    }
    inh->mgr = m;
    inh->inhibitor = wlr_inh;
    inh->destroy.notify = inhibitor_handle_destroy;
    wl_signal_add(&wlr_inh->events.destroy, &inh->destroy);

    m->inhibitors++;
    wlr_idle_notifier_v1_set_inhibited(m->server->idle_notifier, true);
}

/* --- Инициализация --- */

void session_lock_init(struct mywm_server *server) {
    struct mywm_lock_manager *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return;
    }
    m->server = server;
    wl_list_init(&m->locks);

    /* Дерево лок-поверхностей — последним, чтобы быть выше всех;
     * на случай перестроений дополнительно поднимаем при блокировке. */
    m->tree = wlr_scene_tree_create(&server->scene->tree);
    if (m->tree != NULL) {
        wlr_scene_node_set_enabled(&m->tree->node, false);
    }

    m->mgr = wlr_session_lock_manager_v1_create(server->wl_display);
    m->new_lock.notify = lock_handle_new_lock;
    wl_signal_add(&m->mgr->events.new_lock, &m->new_lock);
    server->lock = m;

    /* Буфер обмена для менеджеров (cliphist/copyq): wlr-data-control. */
    wlr_data_control_manager_v1_create(server->wl_display);

    /* Простой: ext-idle-notify + idle-inhibit. */
    server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
    struct wlr_idle_inhibit_manager_v1 *im =
        wlr_idle_inhibit_v1_create(server->wl_display);
    if (im != NULL) {
        m->new_inhibitor.notify = lock_handle_new_inhibitor;
        wl_signal_add(&im->events.new_inhibitor, &m->new_inhibitor);
    }
}
