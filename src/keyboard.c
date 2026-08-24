#define _GNU_SOURCE
#include "server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* Параметры повтора клавиш: rate=25 Гц, delay=600 мс. Без них терминалы
 * не получают зажатые клавиши от клиентов. */
#define KEYBOARD_REPEAT_RATE 25
#define KEYBOARD_REPEAT_DELAY 600
/* Порог (мс), по которому повторное PRESSED того же кода считается
 * автоповтором (интервал повтора = 1000/rate = 40 мс). */
#define KEYBOARD_REPEAT_MIN_INTERVAL 60

#define MOVE_STEP 20
#define RESIZE_STEP 16
#define MIN_WINDOW_W 100
#define MIN_WINDOW_H 60

/* Закрытие сфокусированной view через wlr_xdg_toplevel_send_close —
 * клиент сам решает, как ответить (graceful shutdown). */
static void close_focused_view(struct mywm_server *server) {
    if (server->focused_view != NULL && server->focused_view->mapped) {
        wlr_xdg_toplevel_send_close(server->focused_view->xdg_toplevel);
        wlr_log(WLR_DEBUG, "keybinding: close view=%p",
                (void *)server->focused_view);
    }
}

/*
 * Циклическое переключение фокуса (Alt+Tab / Super+Tab). Ищем следующую
 * mapped view в server->views после сфокусированной (с заворотом на начало).
 * Список хранит порядок создания; Z-порядок и фокус настраивает focus_view
 * (подъём scene-ноды наверх), поэтому окно отрисовывается поверх остальных.
 */
static void cycle_views(struct mywm_server *server) {
    struct mywm_view *view = server->focused_view;
    struct mywm_view *first_mapped = NULL;
    struct mywm_view *it;
    bool found_focused = false;

    wl_list_for_each(it, &server->views, link) {
        if (!it->mapped) {
            continue;
        }
        if (first_mapped == NULL) {
            first_mapped = it;
        }
        if (found_focused) {
            focus_view(server, it, it->xdg_toplevel->base->surface);
            return;
        }
        if (it == view) {
            found_focused = true;
        }
    }
    /* Сфокусированная view не найдена (или нет фокуса) — берём первую mapped. */
    if (first_mapped != NULL) {
        focus_view(server, first_mapped, first_mapped->xdg_toplevel->base->surface);
    }
}

/*
 * Безопасный запуск приложений: fork() + execvp(). В дочернем процессе
 * вызываем setsid() (отвязка от терминала/сессии композитора), закрываем
 * унаследованные файловые дескрипторы и перенаправляем stdio в /dev/null —
 * иначе при выходе приложения композитор может заблокироваться.
 * Разбиение команды на argv выполняется ДО fork: в дочернем процессе
 * остаются только async-signal-safe вызовы.
 */
static void spawn_command(struct mywm_server *server, const char *command) {
    (void)server;
    if (command == NULL || *command == '\0') {
        return;
    }
    char cmd_copy[512];
    char *argv[16];
    size_t len = strlen(command);
    if (len >= sizeof(cmd_copy)) {
        len = sizeof(cmd_copy) - 1;
    }
    memcpy(cmd_copy, command, len);
    cmd_copy[len] = '\0';

    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(cmd_copy, " \t", &save);
            tok != NULL && argc < 15; tok = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0) {
        return;
    }
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        wlr_log(WLR_ERROR, "spawn: fork failed: %s", strerror(errno));
        return;
    }
    if (pid > 0) {
        wlr_log(WLR_DEBUG, "spawn: pid=%d command='%s'", pid, command);
        return;
    }

    /* Дочерний процесс. */
    setsid();
#ifdef SYS_close_range
    syscall(SYS_close_range, 3, ~0U, 0);
#else
    for (int fd = 3; fd < 1024; fd++) {
        close(fd);
    }
#endif
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }
    execvp(argv[0], argv);
    _exit(127);
}

/* Публичная обёртка над spawn_command (клик по закреплённой иконке дока). */
void mywm_spawn(struct mywm_server *server, const char *command) {
    spawn_command(server, command);
}

static void binding_move_view(struct mywm_server *server, int dx, int dy) {
    struct mywm_view *view = server->focused_view;
    if (view == NULL || !view->mapped) {
        return;
    }
    int x = view->x + dx;
    int y = view->y + dy;
    clamp_view_to_layout(server, view, &x, &y);
    effects_view_set_position(view, x, y);
    wlr_log(WLR_DEBUG, "keybinding move: view=%p pos=(%d,%d)",
            (void *)view, x, y);
}

/*
 * Изменение размера сфокусированного окна с клавиатуры: меняем подтверждённый
 * клиентом размер (view->width/height) через wlr_xdg_toplevel_set_size —
 * клиент получает configure и подтверждает новый буфер в commit.
 * Отрицательный шаг (уменьшение) сдвигает левую/верхнюю границу, правая и
 * нижняя остаются на месте.
 */
static void binding_resize_view(struct mywm_server *server, int dw, int dh) {
    struct mywm_view *view = server->focused_view;
    if (view == NULL || !view->mapped) {
        return;
    }
    int w = view->width + dw;
    int h = view->height + dh;
    if (w < MIN_WINDOW_W) {
        w = MIN_WINDOW_W;
    }
    if (h < MIN_WINDOW_H) {
        h = MIN_WINDOW_H;
    }
    dw = w - view->width;
    dh = h - view->height;
    if (dw < 0) {
        view->x += dw;
    }
    if (dh < 0) {
        view->y += dh;
    }
    effects_view_set_position(view, view->x, view->y);
    wlr_xdg_toplevel_set_size(view->xdg_toplevel, w, h);
    wlr_log(WLR_DEBUG, "keybinding resize: view=%p size=%dx%d",
            (void *)view, w, h);
}

static void run_binding(struct mywm_server *server,
                        const struct keybinding *binding) {
    wlr_log(WLR_DEBUG, "keybinding triggered: mods=0x%x keysym=0x%x action=%d",
            binding->modifiers, binding->keysym, binding->action);
    switch (binding->action) {
    case BIND_ACTION_SPAWN:
        spawn_command(server, binding->command);
        break;
    case BIND_ACTION_CLOSE:
        close_focused_view(server);
        break;
    case BIND_ACTION_EXIT:
        wlr_log(WLR_INFO, "keybinding: exiting compositor");
        wl_display_terminate(server->wl_display);
        server->terminate = true;
        break;
    case BIND_ACTION_LOCK:
        spawn_command(server, binding->command);
        break;
    case BIND_ACTION_CYCLE_VIEWS:
        cycle_views(server);
        break;
    case BIND_ACTION_MOVE_UP:
        binding_move_view(server, 0, -MOVE_STEP);
        break;
    case BIND_ACTION_MOVE_DOWN:
        binding_move_view(server, 0, MOVE_STEP);
        break;
    case BIND_ACTION_MOVE_LEFT:
        binding_move_view(server, -MOVE_STEP, 0);
        break;
    case BIND_ACTION_MOVE_RIGHT:
        binding_move_view(server, MOVE_STEP, 0);
        break;
    case BIND_ACTION_RESIZE_UP:
        binding_resize_view(server, 0, -RESIZE_STEP);
        break;
    case BIND_ACTION_RESIZE_DOWN:
        binding_resize_view(server, 0, RESIZE_STEP);
        break;
    case BIND_ACTION_RESIZE_LEFT:
        binding_resize_view(server, -RESIZE_STEP, 0);
        break;
    case BIND_ACTION_RESIZE_RIGHT:
        binding_resize_view(server, RESIZE_STEP, 0);
        break;
    }
}

/*
 * Отличает автоповтор клавиши (wlr_keyboard_set_repeat_info) от нового
 * нажатия: повтор приходит тем же keycode в пределах KEYBOARD_REPEAT_MIN_INTERVAL
 * после предыдущего PRESSED того же кода. Отпускание сбрасывает счётчик.
 */
static bool keyboard_is_repeat(struct mywm_keyboard *keyboard,
                               struct wlr_keyboard_key_event *event) {
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (event->keycode == keyboard->last_repeat_keycode) {
            keyboard->last_repeat_keycode = 0;
            keyboard->last_repeat_time = 0;
        }
        return false;
    }
    if (event->keycode == keyboard->last_repeat_keycode &&
            event->time_msec - keyboard->last_repeat_time <
            KEYBOARD_REPEAT_MIN_INTERVAL) {
        return true;
    }
    keyboard->last_repeat_keycode = event->keycode;
    keyboard->last_repeat_time = event->time_msec;
    return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct mywm_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct mywm_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    if (event == NULL) {
        return;
    }
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state,
                                       keycode, &syms);

    /* Отслеживаем нажатие/отпускание Super — нужно для перемещения окон
     * мышью (Super + перетаскивание). */
    for (int i = 0; i < nsyms; i++) {
        if (syms[i] == XKB_KEY_Super_L || syms[i] == XKB_KEY_Super_R) {
            server->mod_pressed =
                (event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
        }
    }

    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    bool is_repeat = keyboard_is_repeat(keyboard, event);
    bool handled = false;

    /* Esc закрывает открытое меню приложений, клавиша не уходит клиенту. */
    if (apps_menu_is_open(server) &&
            event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            if (syms[i] == XKB_KEY_Escape) {
                apps_menu_toggle(server);
                handled = true;
                break;
            }
        }
    }

    if (nsyms > 0) {
        /* Ищем биндинг по модификаторам (через depressed, а не сырые коды)
         * и любому из символьных значений клавиши (с Shift символы меняются). */
        for (size_t i = 0; i < server->bindings_len && !handled; i++) {
            const struct keybinding *binding = &server->bindings[i];
            if ((modifiers & binding->modifiers) != binding->modifiers) {
                continue;
            }
            bool sym_match = false;
            for (int j = 0; j < nsyms; j++) {
                if (binding->keysym == syms[j]) {
                    sym_match = true;
                    break;
                }
            }
            if (!sym_match) {
                continue;
            }
            if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
                /* Повторы допускаются только для move/resize/cycle. */
                if (is_repeat && !binding->repeatable) {
                    continue;
                }
                run_binding(server, binding);
            }
            /* Нажатие и отпускание забинденной клавиши не уходят клиенту. */
            handled = true;
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec,
                                     event->keycode, event->state);
    }
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct mywm_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    (void)data;
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
                                       &keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct mywm_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    (void)data;
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

/*
 * Компилируем keymap с раскладкой из конфига (config.toml: layout = "us")
 * или системными дефолтами. xkb_context общий для всех клавиатур —
 * создаётся один раз в server_init.
 */
static struct xkb_keymap *create_keymap(struct mywm_server *server) {
    struct xkb_keymap *keymap;
    if (server->keyboard_layout != NULL) {
        struct xkb_rule_names names = {
            .rules = "evdev",
            .model = "pc105",
            .layout = server->keyboard_layout,
        };
        keymap = xkb_keymap_new_from_names(server->xkb_context, &names,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    } else {
        keymap = xkb_keymap_new_from_names(server->xkb_context, NULL,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    if (keymap == NULL) {
        wlr_log(WLR_ERROR, "Failed to compile keymap");
    }
    return keymap;
}

void server_new_keyboard(struct mywm_server *server,
                         struct wlr_keyboard *wlr_keyboard) {
    if (server == NULL || wlr_keyboard == NULL) {
        return;
    }

    struct mywm_keyboard *keyboard = calloc(1, sizeof(struct mywm_keyboard));
    if (keyboard == NULL) {
        return;
    }
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_keymap *keymap = create_keymap(server);
    if (keymap != NULL) {
        wlr_keyboard_set_keymap(wlr_keyboard, keymap);
        xkb_keymap_unref(keymap);
    }
    wlr_keyboard_set_repeat_info(wlr_keyboard,
                                 KEYBOARD_REPEAT_RATE, KEYBOARD_REPEAT_DELAY);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&wlr_keyboard->base.events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
    wlr_log(WLR_DEBUG, "new keyboard: device=%s repeat=%d/%d",
            wlr_keyboard->base.name,
            KEYBOARD_REPEAT_RATE, KEYBOARD_REPEAT_DELAY);
}