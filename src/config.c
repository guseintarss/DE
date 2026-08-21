#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <toml.h>

/*
 * Парсер конфигурации в формате TOML (tomlc99). Формат файла:
 *
 *   layout = "us"                          # раскладка XKB
 *   [bindings]
 *   "super+Return" = "foot"                # запуск команды
 *   "super+Shift+q" = "@close"             # встроенные действия с '@'
 *   "super+Shift+e" = "@exit"
 *   "super+l" = "@lock swaylock"
 *   "super+Tab" = "@cycle"
 *   "super+Left" = "@move left"
 *   "super+Shift+Left" = "@resize left"
 *
 * Сначала применяются хардкод-дефолты (config_load_defaults), затем файл
 * переопределяет их по паре (модификаторы, keysym). Если файл не найден
 * или содержит ошибку — остаются дефолты (graceful fallback).
 */

static struct keybinding *find_binding(struct mywm_server *server,
        uint32_t modifiers, xkb_keysym_t keysym) {
    for (size_t i = 0; i < server->bindings_len; i++) {
        struct keybinding *b = &server->bindings[i];
        if (b->modifiers == modifiers && b->keysym == keysym) {
            return b;
        }
    }
    return NULL;
}

static void set_binding(struct mywm_server *server, uint32_t modifiers,
        xkb_keysym_t keysym, enum binding_action action,
        bool repeatable, const char *command) {
    struct keybinding *b = find_binding(server, modifiers, keysym);
    if (b == NULL) {
        if (server->bindings_len >= MAX_BINDINGS) {
            wlr_log(WLR_ERROR, "config: too many bindings (%d), ignoring %s",
                    MAX_BINDINGS, command != NULL ? command : "builtin");
            return;
        }
        b = &server->bindings[server->bindings_len++];
        b->modifiers = modifiers;
        b->keysym = keysym;
    }
    free(b->command);
    b->action = action;
    b->repeatable = repeatable;
    b->command = command != NULL ? strdup(command) : NULL;
}

void config_load_defaults(struct mywm_server *server) {
    server->bindings_len = 0;
    set_binding(server, WLR_MODIFIER_LOGO, XKB_KEY_Return,
                BIND_ACTION_SPAWN, false, "foot");
    set_binding(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_q,
                BIND_ACTION_CLOSE, false, NULL);
    set_binding(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_e,
                BIND_ACTION_EXIT, false, NULL);
    set_binding(server, WLR_MODIFIER_LOGO, XKB_KEY_l,
                BIND_ACTION_LOCK, false, "swaylock");
    set_binding(server, WLR_MODIFIER_LOGO, XKB_KEY_Tab,
                BIND_ACTION_CYCLE_VIEWS, true, NULL);
    set_binding(server, WLR_MODIFIER_ALT, XKB_KEY_Tab,
                BIND_ACTION_CYCLE_VIEWS, true, NULL);
    set_binding(server, WLR_MODIFIER_ALT, XKB_KEY_F4,
                BIND_ACTION_CLOSE, false, NULL);
    set_binding(server, WLR_MODIFIER_LOGO, XKB_KEY_Left,
                BIND_ACTION_MOVE_LEFT, true, NULL);
    set_binding(server, WLR_MODIFIER_LOGO, XKB_KEY_Right,
                BIND_ACTION_MOVE_RIGHT, true, NULL);
    set_binding(server, WLR_MODIFIER_LOGO, XKB_KEY_Up,
                BIND_ACTION_MOVE_UP, true, NULL);
    set_binding(server, WLR_MODIFIER_LOGO, XKB_KEY_Down,
                BIND_ACTION_MOVE_DOWN, true, NULL);
    set_binding(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Left,
                BIND_ACTION_RESIZE_LEFT, true, NULL);
    set_binding(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Right,
                BIND_ACTION_RESIZE_RIGHT, true, NULL);
    set_binding(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Up,
                BIND_ACTION_RESIZE_UP, true, NULL);
    set_binding(server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_Down,
                BIND_ACTION_RESIZE_DOWN, true, NULL);
}

/* "super", "shift", "ctrl", "alt" -> бит WLR_MODIFIER_*.
 * Имена модификаторов регистронезависимы ("Shift", "SUPER", ...). */
static bool parse_modifier_name(const char *name, uint32_t *mod) {
    if (strcasecmp(name, "super") == 0 || strcasecmp(name, "logo") == 0 ||
            strcasecmp(name, "mod4") == 0) {
        *mod = WLR_MODIFIER_LOGO;
    } else if (strcasecmp(name, "shift") == 0) {
        *mod = WLR_MODIFIER_SHIFT;
    } else if (strcasecmp(name, "ctrl") == 0 || strcasecmp(name, "control") == 0) {
        *mod = WLR_MODIFIER_CTRL;
    } else if (strcasecmp(name, "alt") == 0 || strcasecmp(name, "mod1") == 0) {
        *mod = WLR_MODIFIER_ALT;
    } else {
        return false;
    }
    return true;
}

/* "super+shift+q" -> modifiers=LOGO|SHIFT, keysym=XKB_KEY_q. */
static bool parse_binding_key(const char *key, uint32_t *modifiers,
                              xkb_keysym_t *keysym) {
    *modifiers = 0;
    *keysym = XKB_KEY_NoSymbol;
    char *copy = strdup(key);
    if (copy == NULL) {
        return false;
    }
    char *save = NULL;
    bool ok = false;
    for (char *part = strtok_r(copy, "+", &save);
            part != NULL; part = strtok_r(NULL, "+", &save)) {
        uint32_t mod;
        if (parse_modifier_name(part, &mod)) {
            *modifiers |= mod;
        } else {
            *keysym = xkb_keysym_from_name(part, XKB_KEYSYM_CASE_INSENSITIVE);
            if (*keysym == XKB_KEY_NoSymbol) {
                wlr_log(WLR_ERROR, "config: unknown keysym '%s'", part);
                ok = false;
                goto out;
            }
            ok = true;
        }
    }
out:
    free(copy);
    return ok && *keysym != XKB_KEY_NoSymbol;
}

/*
 * Значение биндинга: либо команда запуска ("foot -e htop"), либо
 * встроенное действие с префиксом '@': @close, @exit, @cycle,
 * @lock [cmd], @move <dir>, @resize <dir>.
 */
static bool parse_binding_value(const char *value, enum binding_action *action,
                                bool *repeatable, const char **command) {
    *action = BIND_ACTION_SPAWN;
    *repeatable = false;
    *command = NULL;
    if (value[0] != '@') {
        *command = value;
        return true;
    }
    const char *cmd = value + 1;
    if (strcmp(cmd, "close") == 0) {
        *action = BIND_ACTION_CLOSE;
        return true;
    }
    if (strcmp(cmd, "exit") == 0) {
        *action = BIND_ACTION_EXIT;
        return true;
    }
    if (strcmp(cmd, "cycle") == 0) {
        *action = BIND_ACTION_CYCLE_VIEWS;
        *repeatable = true;
        return true;
    }
    if (strncmp(cmd, "lock", 4) == 0 && (cmd[4] == '\0' || cmd[4] == ' ')) {
        *action = BIND_ACTION_LOCK;
        *command = cmd[4] == ' ' ? cmd + 5 : "swaylock";
        return true;
    }
    if (strncmp(cmd, "move", 4) == 0 && cmd[4] == ' ') {
        const char *dir = cmd + 5;
        *repeatable = true;
        if (strcmp(dir, "up") == 0) {
            *action = BIND_ACTION_MOVE_UP;
        } else if (strcmp(dir, "down") == 0) {
            *action = BIND_ACTION_MOVE_DOWN;
        } else if (strcmp(dir, "left") == 0) {
            *action = BIND_ACTION_MOVE_LEFT;
        } else if (strcmp(dir, "right") == 0) {
            *action = BIND_ACTION_MOVE_RIGHT;
        } else {
            wlr_log(WLR_ERROR, "config: unknown move direction '%s'", dir);
            return false;
        }
        return true;
    }
    if (strncmp(cmd, "resize", 6) == 0 && cmd[6] == ' ') {
        const char *dir = cmd + 7;
        *repeatable = true;
        if (strcmp(dir, "up") == 0) {
            *action = BIND_ACTION_RESIZE_UP;
        } else if (strcmp(dir, "down") == 0) {
            *action = BIND_ACTION_RESIZE_DOWN;
        } else if (strcmp(dir, "left") == 0) {
            *action = BIND_ACTION_RESIZE_LEFT;
        } else if (strcmp(dir, "right") == 0) {
            *action = BIND_ACTION_RESIZE_RIGHT;
        } else {
            wlr_log(WLR_ERROR, "config: unknown resize direction '%s'", dir);
            return false;
        }
        return true;
    }
    wlr_log(WLR_ERROR, "config: unknown action '@%s'", cmd);
    return false;
}

/* Дефолты секций [wallpaper]/[animations] (переопределяются config.toml). */
void config_anim_defaults(struct mywm_server *server) {
    server->wallpaper_cfg.path = NULL;
    server->wallpaper_cfg.mode = WALLPAPER_MODE_COVER;
    server->animations_cfg.enabled = true;
    server->animations_cfg.stiffness = EFFECTS_SPRING_STIFFNESS;
    server->animations_cfg.damping = EFFECTS_SPRING_DAMPING;
    server->animations_cfg.open_slide = EFFECTS_OPEN_SLIDE;
    server->animations_cfg.close_slide = EFFECTS_CLOSE_SLIDE;
}

static enum wallpaper_mode parse_wallpaper_mode(const char *s) {
    if (strcmp(s, "stretch") == 0) {
        return WALLPAPER_MODE_STRETCH;
    }
    if (strcmp(s, "fit") == 0) {
        return WALLPAPER_MODE_FIT;
    }
    if (strcmp(s, "tile") == 0) {
        return WALLPAPER_MODE_TILE;
    }
    return WALLPAPER_MODE_COVER;
}

static void config_parse_wallpaper(struct mywm_server *server,
                                   toml_table_t *tab) {
    toml_datum_t path = toml_string_in(tab, "path");
    if (path.ok) {
        free(server->wallpaper_cfg.path);
        server->wallpaper_cfg.path = strdup(path.u.s);
        free(path.u.s);
        wlr_log(WLR_INFO, "config: wallpaper path = '%s'",
                server->wallpaper_cfg.path);
    }
    toml_datum_t mode = toml_string_in(tab, "mode");
    if (mode.ok) {
        server->wallpaper_cfg.mode = parse_wallpaper_mode(mode.u.s);
        free(mode.u.s);
        wlr_log(WLR_INFO, "config: wallpaper mode = %d",
                server->wallpaper_cfg.mode);
    }
}

static void config_parse_animations(struct mywm_server *server,
                                    toml_table_t *tab) {
    toml_datum_t enabled = toml_bool_in(tab, "enabled");
    if (enabled.ok) {
        server->animations_cfg.enabled = enabled.u.b;
    }
    toml_datum_t stiffness = toml_double_in(tab, "stiffness");
    if (stiffness.ok && stiffness.u.d > 0.0) {
        server->animations_cfg.stiffness = stiffness.u.d;
    }
    toml_datum_t damping = toml_double_in(tab, "damping");
    if (damping.ok && damping.u.d > 0.0) {
        server->animations_cfg.damping = damping.u.d;
    }
    toml_datum_t open_slide = toml_int_in(tab, "open_slide");
    if (open_slide.ok) {
        server->animations_cfg.open_slide = (int)open_slide.u.i;
    }
    toml_datum_t close_slide = toml_int_in(tab, "close_slide");
    if (close_slide.ok) {
        server->animations_cfg.close_slide = (int)close_slide.u.i;
    }
    wlr_log(WLR_INFO, "config: animations enabled=%d stiffness=%.0f "
            "damping=%.0f open_slide=%d close_slide=%d",
            server->animations_cfg.enabled,
            server->animations_cfg.stiffness,
            server->animations_cfg.damping,
            server->animations_cfg.open_slide,
            server->animations_cfg.close_slide);
}

void config_load(struct mywm_server *server, const char *path) {
    if (path == NULL) {
        return;
    }
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        wlr_log(WLR_INFO, "config: '%s' not found, using defaults", path);
        return;
    }
    char errbuf[200];
    toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);
    if (root == NULL) {
        wlr_log(WLR_ERROR, "config: '%s': parse error: %s", path, errbuf);
        return;
    }

    toml_datum_t layout = toml_string_in(root, "layout");
    if (layout.ok) {
        free(server->keyboard_layout);
        server->keyboard_layout = strdup(layout.u.s);
        free(layout.u.s);
        wlr_log(WLR_INFO, "config: keyboard layout = '%s'",
                server->keyboard_layout);
    }

    toml_table_t *bindings = toml_table_in(root, "bindings");
    if (bindings != NULL) {
        int n = toml_table_nkval(bindings);
        for (int i = 0; i < n; i++) {
            const char *key = toml_key_in(bindings, i);
            toml_datum_t val = toml_string_in(bindings, key);
            if (!val.ok) {
                wlr_log(WLR_ERROR, "config: binding '%s' is not a string, "
                        "skipping", key);
                continue;
            }
            uint32_t mods;
            xkb_keysym_t keysym;
            enum binding_action action;
            bool repeatable;
            const char *command;
            if (parse_binding_key(key, &mods, &keysym) &&
                    parse_binding_value(val.u.s, &action, &repeatable,
                                        &command)) {
                set_binding(server, mods, keysym, action, repeatable, command);
                wlr_log(WLR_DEBUG, "config: binding '%s' -> action=%d cmd='%s'",
                        key, action, command != NULL ? command : "");
            }
            free(val.u.s);
        }
    }
    toml_table_t *wallpaper = toml_table_in(root, "wallpaper");
    if (wallpaper != NULL) {
        config_parse_wallpaper(server, wallpaper);
    }
    toml_table_t *animations = toml_table_in(root, "animations");
    if (animations != NULL) {
        config_parse_animations(server, animations);
    }

    toml_free(root);
    wlr_log(WLR_INFO, "config: '%s' loaded, %zu bindings active",
            path, server->bindings_len);
}

/*
 * Поиск файла конфигурации: $DE_CONFIG -> $XDG_CONFIG_HOME/de/config.toml
 * -> ~/.config/de/config.toml -> config.toml (текущий каталог).
 * Если ни один не найден — остаются дефолты.
 */
void config_load_auto(struct mywm_server *server) {
    const char *env = getenv("DE_CONFIG");
    if (env != NULL) {
        config_load(server, env);
        return;
    }
    char path[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg != NULL) {
        snprintf(path, sizeof(path), "%s/de/config.toml", xdg);
    } else if (home != NULL) {
        snprintf(path, sizeof(path), "%s/.config/de/config.toml", home);
    } else {
        path[0] = '\0';
    }
    if (path[0] != '\0' && access(path, R_OK) == 0) {
        config_load(server, path);
        return;
    }
    config_load(server, "config.toml");
}

void config_finish(struct mywm_server *server) {
    for (size_t i = 0; i < server->bindings_len; i++) {
        free(server->bindings[i].command);
    }
    server->bindings_len = 0;
    free(server->keyboard_layout);
    server->keyboard_layout = NULL;
    free(server->wallpaper_cfg.path);
    server->wallpaper_cfg.path = NULL;
}