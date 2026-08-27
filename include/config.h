#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_BINDINGS 32

enum binding_action {
    BIND_ACTION_SPAWN,          /* запуск команды (binding->command) */
    BIND_ACTION_CLOSE,          /* закрыть сфокусированное окно */
    BIND_ACTION_EXIT,           /* выйти из композитора */
    BIND_ACTION_LOCK,           /* запустить экранный блокировщик */
    BIND_ACTION_CYCLE_VIEWS,    /* переключение фокуса между окнами */
    BIND_ACTION_MOVE_UP,
    BIND_ACTION_MOVE_DOWN,
    BIND_ACTION_MOVE_LEFT,
    BIND_ACTION_MOVE_RIGHT,
    BIND_ACTION_RESIZE_UP,
    BIND_ACTION_RESIZE_DOWN,
    BIND_ACTION_RESIZE_LEFT,
    BIND_ACTION_RESIZE_RIGHT,
    /* GNOME-стиль управления окном: тайлинг к половинам и максимизация. */
    BIND_ACTION_TILE_LEFT,      /* Super+Left: левая половина */
    BIND_ACTION_TILE_RIGHT,     /* Super+Right: правая половина */
    BIND_ACTION_VIEW_MAXIMIZE,  /* Super+Up: максимизация */
    BIND_ACTION_VIEW_RESTORE,   /* Super+Down: восстановить/свернуть */
    /* Рабочие столы (Spaces): arg — индекс назначения (0-based) для
     * SWITCH/MOVE. */
    BIND_ACTION_WS_SWITCH,      /* @ws <n>: перейти на стол n */
    BIND_ACTION_WS_NEXT,        /* @ws-next */
    BIND_ACTION_WS_PREV,        /* @ws-prev */
    BIND_ACTION_WS_MOVE,        /* @ws-move <n>: окно -> стол n (и следом) */
    BIND_ACTION_WS_MOVE_NEXT,   /* @ws-move-next */
    BIND_ACTION_WS_MOVE_PREV,   /* @ws-move-prev */
};

/* Число воркспейсов (Spaces). Индексы 0..WS_MAX-1, создаются по demand. */
#define WS_MAX 9

struct keybinding {
    /* Маска WLR_MODIFIER_* (wlr/types/wlr_keyboard.h): какие модификаторы
     * должны быть зажаты, чтобы биндинг сработал. */
    uint32_t modifiers;
    /* Клавиша в терминах XKB (например, XKB_KEY_q, XKB_KEY_Return). */
    xkb_keysym_t keysym;
    enum binding_action action;
    /* Допускает срабатывание на повторе клавиши (move/resize/cycle).
     * spawn/close/exit при зажатой клавише срабатывают один раз. */
    bool repeatable;
    /* Команда для BIND_ACTION_SPAWN/LOCK (argv без шелла), иначе NULL. */
    char *command;
    /* Числовой аргумент действия (@ws 3 -> 2, 0-based). */
    int arg;
};

/* --- Этап 5: эффекты --- */

enum wallpaper_mode {
    WALLPAPER_MODE_COVER,   /* заполнить экран с обрезкой (по умолчанию) */
    WALLPAPER_MODE_STRETCH, /* растянуть без сохранения пропорций */
    WALLPAPER_MODE_FIT,     /* вписать целиком, по бокам цвет фона */
    WALLPAPER_MODE_TILE,    /* повторить исходный размер сеткой */
};

struct wallpaper_config {
    /* Путь к файлу обоев (NULL/пусто — встроенные из assets/). */
    char *path;
    enum wallpaper_mode mode;
};

/*
 * Оболочка ([shell] в config.toml). builtin=true — менюбар и док рисует
 * сам композитор (bar.c/dock.c); builtin=false — встроенная оболочка
 * выключена, поверх работают внешние клиенты (waybar, AGSv2, QuickShell)
 * через zwlr-layer-shell.
 */
struct shell_config {
    bool builtin;
    /* Команда внешней оболочки, запускаемая автоматически при старте
     * композитора (только при builtin=false). Пусто — не запускать. */
    char *start;
};

struct animations_config {
    /* Spring-анимации открытия/закрытия окон. */
    bool enabled;
    /* Параметры spring-физики (см. EFFECTS_SPRING_* в effects.h). */
    double stiffness;
    double damping;
    /* Сдвиг по Y при открытии (вниз) и закрытии (вверх), px. */
    int open_slide;
    int close_slide;
};

/*
 * Ввод ([input] в config.toml): настройка тачпадов через libinput.
 * macOS-дефолты: tap-to-click и natural scroll включены.
 */
struct input_config {
    bool tap_to_click;
    bool natural_scroll;
    bool disable_while_typing;
    /* Ускорение указателя, -1..1 (0 — как в системе). */
    double accel_speed;
};

/*
 * Дизайн оболочки ([design] в config.toml). Единственный источник
 * цветов/метрик для менюбара, дока и декораций окон. Меняется на лету:
 * kill -HUP <pid композитора> перечитывает секцию и применяет её.
 */
struct design_config {
    /* Семейство шрифта менюбара (cairo). */
    char *font;

    /* Окно: рамка, заголовок, кнопки управления; высота менюбара. */
    int border;
    int title_h;
    int btn_size;
    int btn_gap;
    int menu_bar_h;

    /* Док: размер иконки, внутренние отступы и зазор между иконками. */
    int dock_icon;
    int dock_pad;
    int dock_gap;

    /* Цвета — прямые RGBA [0..1] (НЕ premultiplied). Парсинг: hex-строка
     * "#RRGGBB" или "#RRGGBBAA". */
    float window_border[4];
    float window_body[4];
    float title_focused[4];
    float title_unfocused[4];
    float border_hover[4];
    float btn_close[4];
    float btn_minimize[4];
    float btn_maximize[4];
    float bar_bg[4];
    float bar_line[4];
    float bar_text[4];
    float bar_icon[4];
    float dock_bg[4];
    float dock_idle[4];
    float dock_active[4];
    float dock_minimized[4];
    float dock_sep[4];
    float dock_dot[4];

    /* Скругление углов окна (px): обычного и максимизированного.
     * 0 — прямые углы. */
    double corner_radius;
    double corner_radius_maximized;
};

struct mywm_server;

void config_load_defaults(struct mywm_server *server);
/* Дефолты для [wallpaper]/[animations] (вызывается до config_load_auto). */
void config_anim_defaults(struct mywm_server *server);
/* Дефолты [shell] (вызывается до config_load_auto). */
void config_shell_defaults(struct mywm_server *server);
/* Дефолты [input] (вызывается до config_load_auto). */
void config_input_defaults(struct mywm_server *server);
/* Дефолты [design] (вызывается до config_load_auto, до создания UI). */
void config_design_defaults(struct mywm_server *server);
/* Перечитать только [design] из авто-пути (по SIGHUP) и применить к живым
 * нодам сцены. Возвращает false, если файл не читается. */
bool config_design_reload(struct mywm_server *server);
void config_load(struct mywm_server *server, const char *path);
void config_load_auto(struct mywm_server *server);
void config_finish(struct mywm_server *server);

#endif