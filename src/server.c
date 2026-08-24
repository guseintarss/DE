#include "server.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

static void output_frame_handler(struct wl_listener *listener, void *data) {
    struct mywm_output *output = wl_container_of(listener, output, frame);
    (void)data;

    wallpaper_apply(output->server, output);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_commit(output->scene_output, NULL);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_destroy_handler(struct wl_listener *listener, void *data) {
    struct mywm_output *output = wl_container_of(listener, output, destroy);
    (void)data;
    if (output->background != NULL) {
        wlr_scene_node_destroy(&output->background->node);
    }
    for (int i = 0; i < output->tile_count; i++) {
        if (output->tiles[i] != NULL) {
            wlr_scene_node_destroy(&output->tiles[i]->node);
        }
    }
    if (output->background_fallback != NULL) {
        wlr_scene_node_destroy(&output->background_fallback->node);
    }
    if (output->bg_tree != NULL) {
        wlr_scene_node_destroy(&output->bg_tree->node);
    }
    wlr_scene_output_destroy(output->scene_output);
    wl_list_remove(&output->link);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    /* В wlroots 0.17+ новый вывод создаётся выключенным: без явного
     * enabled=true кадры (events.frame) не приходят никогда. */
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "Failed to commit output state: %s", wlr_output->name);
    }
    wlr_output_state_finish(&state);

    struct mywm_output *output = calloc(1, sizeof(struct mywm_output));
    if (output == NULL) {
        return;
    }
    output->wlr_output = wlr_output;
    output->server = server;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    if (output->scene_output == NULL) {
        free(output);
        return;
    }

    /* Субдерево обоев: самый нижний слой (ниже всех окон). */
    output->bg_tree = wlr_scene_tree_create(&server->scene->tree);
    wlr_scene_node_lower_to_bottom(&output->bg_tree->node);

    struct wlr_scene_node *background_node;
    if (server->wallpaper != NULL) {
        output->background = wlr_scene_buffer_create(
                output->bg_tree, server->wallpaper->buffer);
        background_node = &output->background->node;
    } else {
        float bg_color[4] = {0.08f, 0.09f, 0.16f, 1.0f};
        output->background_fallback = wlr_scene_rect_create(
                output->bg_tree,
                wlr_output->width, wlr_output->height,
                bg_color);
        background_node = &output->background_fallback->node;
    }
    (void)background_node;

    output->frame.notify = output_frame_handler;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = output_destroy_handler;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);
    wlr_output_layout_add_auto(server->output_layout, wlr_output);

    /* Оболочка привязана к layout: пересчитать док и менюбар под новый
     * выход (dock_init/bar_init выполняются до появления мониторов). */
    dock_refresh(server);
    bar_update_name(server);

    wlr_log(WLR_INFO, "New output: %s (%dx%d)",
            wlr_output->name, wlr_output->width, wlr_output->height);
}

static void update_seat_caps(struct mywm_server *server) {
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct mywm_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(server, wlr_keyboard_from_input_device(device));
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(server, device);
        break;
    default:
        break;
    }
    update_seat_caps(server);
}

/*
 * Виртуальные устройства (для headless-тестирования, без реального железа).
 * Менеджеры созданы в server_init; здесь мы принимаем их объекты и пускаем
 * по тому же пути, что и реальные устройства (server_new_keyboard/pointer).
 */
static void server_new_virtual_keyboard(struct wl_listener *listener, void *data) {
    struct mywm_server *server =
        wl_container_of(listener, server, new_virtual_keyboard);
    struct wlr_virtual_keyboard_v1 *vk = data;
    struct wlr_input_device *kb_device = &vk->keyboard.base;
    if (kb_device) {
        server_new_keyboard(server, wlr_keyboard_from_input_device(kb_device));
        update_seat_caps(server);
    }
}

static void server_new_virtual_pointer(struct wl_listener *listener, void *data) {
    struct mywm_server *server =
        wl_container_of(listener, server, new_virtual_pointer);
    struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
    struct wlr_input_device *ptr_device = &event->new_pointer->pointer.base;
    if (ptr_device) {
        server_new_pointer(server, ptr_device);
        update_seat_caps(server);
    }
}

static void find_headless_backend(struct wlr_backend *backend, void *data) {
    if (wlr_backend_is_headless(backend)) {
        *(struct wlr_backend **)data = backend;
    }
}

static void add_headless_outputs(struct mywm_server *server) {
    const char *headless_outputs = getenv("WLR_HEADLESS_OUTPUTS");
    if (headless_outputs == NULL) {
        return;
    }
    int count = atoi(headless_outputs);
    if (count <= 0) {
        return;
    }
    struct wlr_backend *headless = NULL;
    if (wlr_backend_is_multi(server->backend)) {
        wlr_multi_for_each_backend(server->backend, find_headless_backend, &headless);
    } else if (wlr_backend_is_headless(server->backend)) {
        headless = server->backend;
    }
    if (headless == NULL) {
        wlr_log(WLR_ERROR, "WLR_HEADLESS_OUTPUTS set but no headless backend found");
        return;
    }
    for (int i = 0; i < count; i++) {
        wlr_headless_add_output(headless, 1280, 720);
    }
}

/* SIGHUP: перечитать [design] и применить к живой сцене. */
static int handle_sighup(int signal_number, void *data) {
    struct mywm_server *server = data;
    if (signal_number != SIGHUP) {
        return 0;
    }
    if (!config_design_reload(server)) {
        wlr_log(WLR_ERROR, "design reload failed, keeping previous design");
        return 0;
    }
    design_apply(server);
    return 0;
}

void server_init(struct mywm_server *server) {
    wlr_log_init(WLR_DEBUG, NULL);

    server->wl_display = wl_display_create();
    server->backend = wlr_backend_autocreate(
            wl_display_get_event_loop(server->wl_display), NULL);
    server->renderer = wlr_renderer_autocreate(server->backend);
    wlr_renderer_init_wl_display(server->renderer, server->wl_display);

    add_headless_outputs(server);

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    server->compositor = wlr_compositor_create(server->wl_display, 6,
                                               server->renderer);
    server->output_layout = wlr_output_layout_create(server->wl_display);
    server->scene = wlr_scene_create();

    server->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (server->xkb_context == NULL) {
        wlr_log(WLR_ERROR, "Failed to create xkb_context");
        exit(1);
    }
    /* Конфиг грузится до обоев: [wallpaper].path влияет на выбор файла. */
    config_load_defaults(server);
    config_anim_defaults(server);
    config_load_auto(server);

    server->wallpaper = wallpaper_load_auto(server);
    animations_init(server);

    /* Инициализация менеджера иконок */
    icon_manager_init(&server->icon_mgr, server->renderer);

    wl_list_init(&server->outputs);
    wl_list_init(&server->views);
    wl_list_init(&server->keyboards);
    server->cursor_mode = MYWM_CURSOR_PASSTHROUGH;

    server->seat = wlr_seat_create(server->wl_display, "seat0");

    /* SIGHUP: перечитать [design] из config.toml и применить на лету
     * (pkill -HUP -x DE). */
    struct wl_event_loop *init_loop =
        wl_display_get_event_loop(server->wl_display);
    wl_event_loop_add_signal(init_loop, SIGHUP, handle_sighup, server);

    /* Модули этапа 3: курсор (ввод) и xdg-shell (окна). Создают свои
     * wlr-объекты и регистрируют listeners. */
    cursor_init(server);
    xdg_shell_init(server);
    dock_init(server);
    bar_init(server);
    apps_menu_init(server);

    server->new_output.notify = server_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);
    server->new_input.notify = server_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);

    struct wlr_virtual_keyboard_manager_v1 *vk_mgr =
        wlr_virtual_keyboard_manager_v1_create(server->wl_display);
    server->new_virtual_keyboard.notify = server_new_virtual_keyboard;
    wl_signal_add(&vk_mgr->events.new_virtual_keyboard, &server->new_virtual_keyboard);

    struct wlr_virtual_pointer_manager_v1 *vp_mgr =
        wlr_virtual_pointer_manager_v1_create(server->wl_display);
    server->new_virtual_pointer.notify = server_new_virtual_pointer;
    wl_signal_add(&vp_mgr->events.new_virtual_pointer, &server->new_virtual_pointer);

    const char *socket = wl_display_add_socket_auto(server->wl_display);
    if (socket == NULL) {
        wlr_log(WLR_ERROR, "Failed to add socket");
        wl_display_destroy(server->wl_display);
        exit(1);
    }
    setenv("WAYLAND_DISPLAY", socket, true);
    printf("Running MyWM on Wayland display: %s\n", socket);
}

void server_run(struct mywm_server *server) {
    if (!wlr_backend_start(server->backend)) {
        wl_display_destroy(server->wl_display);
        exit(1);
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    while (!server->terminate) {
        wl_event_loop_dispatch(loop, 100);
        wl_display_flush_clients(server->wl_display);
    }
}

void server_finish(struct mywm_server *server) {
    animations_finish(server);
    wallpaper_destroy(server->wallpaper);
    icon_manager_finish(&server->icon_mgr);
    config_finish(server);
    xkb_context_unref(server->xkb_context);
    wl_display_destroy_clients(server->wl_display);
    wl_display_destroy(server->wl_display);
}