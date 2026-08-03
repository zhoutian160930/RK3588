#pragma once
#include "app_config.h"
#include "frame_bus.h"

/* UI 模式：在 UI 主线程中调用。启动一个后台工作线程跑推理管线，
 * 把每帧合成结果(带检测框) push 到 bus；同时驱动 LVGL 主循环把帧显示到 canvas。
 * 阻塞直到管线结束。返回 0 表示成功。 */
int run_ui_mode(const AppConfig &cfg);
