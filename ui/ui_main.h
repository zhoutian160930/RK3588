#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 LVGL + 显示后端(SDL/fbdev) + 鼠标/键盘输入。成功返回 0。 */
int ui_init(int width, int height);

/* 推进 LVGL 一次（由宿主主循环周期性调用，约 5ms 一次）。 */
void ui_tick(void);

/* LVGL tick 时间源（lv_conf.h 中 LV_TICK_CUSTOM_SYS_TIME_EXPR 引用）。 */
uint32_t custom_tick_get(void);

/* 小号中文字体(16px)，供按钮/日志等紧凑控件使用；可能返回 NULL(用默认)。 */
lv_font_t *ui_font_small(void);

#ifdef __cplusplus
}
#endif
