#ifndef EFFECTS_H
#define EFFECTS_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct mywm_server;
struct mywm_view;

/* Один глобальный timer на композитор (~16 мс) ведёт spring-анимации
 * всех окон: opacity, slide (Y), hover и трансформации (maximize/genie). */
#define EFFECTS_ANIM_INTERVAL_MS 16

/* Параметры spring-физики в стиле macOS (iOS spring):
 * velocity += (target - current) * stiffness * dt
 * velocity -= velocity * damping * dt
 * current += velocity * dt
 * damping 26 при stiffness 170 даёт ~критическое затухание. */
#define EFFECTS_SPRING_STIFFNESS 170.0
#define EFFECTS_SPRING_DAMPING 26.0
/* Ускоренная пружина для hover (отзывчивость). */
#define EFFECTS_HOVER_STIFFNESS 400.0
#define EFFECTS_HOVER_DAMPING 35.0

/* Сдвиг вниз при открытии окна (px). */
#define EFFECTS_OPEN_SLIDE 14
/* Сдвиг вверх при закрытии (px). */
#define EFFECTS_CLOSE_SLIDE 12

/* Масштаб окна в начале открытия и в конце закрытия (zoom macOS).
 * Эмулируется через dest_size хрома и содержимого вокруг центра. */
#define EFFECTS_OPEN_SCALE 0.92
#define EFFECTS_CLOSE_SCALE 0.94

/* Жёсткая пружина трансформаций (maximize/unmaximize/genie): визуально
 * завершается за ~0.3 с, как зелёная кнопка в macOS. */
#define EFFECTS_TFORM_STIFFNESS 700.0
#define EFFECTS_TFORM_DAMPING 53.0

/* Скругление углов окна (px) и genie-сворачивание в док (целевой
 * масштаб и непрозрачность). */
#define EFFECTS_CORNER_RADIUS 12.0
#define EFFECTS_GENIE_SCALE 0.15
#define EFFECTS_GENIE_OPACITY 0.20

struct spring_anim {
    bool active;
    double current, target, velocity;
    double stiffness, damping;
};

void spring_init(struct spring_anim *s, double stiffness, double damping);
void spring_set_target(struct spring_anim *s, double target);
bool spring_step(struct spring_anim *s, double dt);
bool spring_settled(const struct spring_anim *s);

/* --- animation.c --- */
/* Единый глобальный таймер анимаций: создаётся в server_init,
 * удаляется в server_finish. */
void animations_init(struct mywm_server *server);
void animations_finish(struct mywm_server *server);
/* Запуск/остановка анимации конкретного view (открытие, закрытие, hover). */
void view_effects_start_anim(struct mywm_view *view);
void view_effects_stop_anim(struct mywm_view *view);
void view_effects_open(struct mywm_view *view);
void view_effects_close(struct mywm_view *view);
void view_effects_hover(struct mywm_view *view, bool hovered);
/* Установить базовую позицию окна (применяет активный slide). */
void effects_view_set_position(struct mywm_view *view, int x, int y);

/* --- Трансформации окна (как зелёная кнопка в macOS) --- */
enum mywm_tform_kind {
    TFORM_NONE = 0,
    TFORM_MAXIMIZE,    /* развернуть: пружина роста до границ layout */
    TFORM_UNMAXIMIZE,  /* вернуть из максимизации */
    TFORM_GENIE_IN,    /* свернуть: окно "втягивается" в иконку дока */
    TFORM_GENIE_OUT,   /* восстановить из иконки дока */
};

/* Геометрия трансформации: позиция deco_tree, размер хрома (текстуры),
 * размер содержимого и непрозрачность. Между tform_a и tform_b тик
 * интерполирует по прогрессу пружины. */
struct tform_geo {
    double x, y;
    double cw, ch;
    double w, h;
    double op;
};

/* --- effects.c (хром окна + трансформации) --- */
/* Пересоздаёт CPU-текстуру хрома (рамка+заголовок+тело) с прозрачными
 * скруглёнными углами. Цвет рамки интерполируется hover-пружиной,
 * заголовок — фокусом. Во время активной трансформации не вызывается
 * (размеры ведёт анимация; финализация пересоздаст сама). */
void effects_chrome_regen(struct mywm_view *view);
/* Тень под окном: общая размытая текстура, геометрия считается вызывающим
 * (dx,dy — позиция ноды, dw,dh — dest_size, alpha 0..1). */
#define EFFECTS_SHADOW_MARGIN 28
#define EFFECTS_SHADOW_BIAS 6
void effects_shadow_place(struct mywm_view *view, int dx, int dy,
                          int dw, int dh, float alpha);
/* Тень в естественное положение вокруг текущего хрома. */
void effects_shadow_reset_alpha(struct mywm_view *view, float alpha);
/* Старт трансформации kind. Останавливает open/close-пружины. */
void effects_tform_start(struct mywm_view *view, enum mywm_tform_kind kind);
/* Применение интерполяции по прогрессу пружины (вызывается из тика). */
void effects_tform_apply(struct mywm_view *view);
/* Завершение: точная геометрия, пересоздание хрома, смена состояния. */
void effects_tform_finalize(struct mywm_view *view);
/* Отмена (например, перед анимацией закрытия): возврат узлов в норму. */
void effects_tform_cancel(struct mywm_view *view);

#endif