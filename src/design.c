#include "server.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/*
 * Применение [design] к живым нодам сцены (после config_design_reload):
 * декорации окон (цвета rect'ов), кнопки заголовков, хром, менюбар и док.
 */
void design_apply(struct mywm_server *server) {
    const struct design_config *d = &server->design;
    struct mywm_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->deco_tree == NULL) {
            continue;
        }
        /* Цвета декораций: бордюр, тело, заголовок (по фокусу). */
        if (view->deco_border != NULL) {
            wlr_scene_rect_set_color(view->deco_border, d->window_border);
        }
        if (view->deco_body != NULL) {
            wlr_scene_rect_set_color(view->deco_body, d->window_body);
        }
        if (view->deco_title != NULL) {
            const float *title =
                server->focused_view == view ?
                d->title_focused : d->title_unfocused;
            wlr_scene_rect_set_color(view->deco_title, title);
        }

        /* Кнопки: пересоздание новым размером/цветом и перепозиционирование
         * (та же схема, что при создании view в xdg_shell.c). */
        const float *btn_colors[3] = {d->btn_close, d->btn_minimize,
                                      d->btn_maximize};
        for (int i = 0; i < 3; i++) {
            mywm_btn_recreate(view->deco_tree, &view->btns[i],
                              d->btn_size, btn_colors[i],
                              (enum mywm_title_button)(i + 1));
            wlr_scene_node_set_position(
                &view->btns[i].node->node,
                d->border + BTN_X + i * (d->btn_size + d->btn_gap),
                (d->title_h - d->btn_size) / 2);
        }

        /* Хром: сброс кэша размера — effects_chrome_regen перерисует
         * текстуру новыми цветами/метриками. */
        view->chrome_w = 0;
        view->chrome_h = 0;
        effects_chrome_regen(view);
    }

    bar_redesign(server);
    dock_redesign(server);
    wlr_log(WLR_INFO, "design applied: border=%d title_h=%d btn=%d",
            d->border, d->title_h, d->btn_size);
}
