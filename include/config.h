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
};

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

struct mywm_server;

void config_load_defaults(struct mywm_server *server);
/* Дефолты для [wallpaper]/[animations] (вызывается до config_load_auto). */
void config_anim_defaults(struct mywm_server *server);
void config_load(struct mywm_server *server, const char *path);
void config_load_auto(struct mywm_server *server);
void config_finish(struct mywm_server *server);

#endif