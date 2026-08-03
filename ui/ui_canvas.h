#pragma once

#include <opencv2/core.hpp>
#include "lvgl.h"

/* 创建一个全屏 canvas 用于显示视频帧。返回 canvas 对象。 */
lv_obj_t *ui_canvas_create(lv_obj_t *parent, int width, int height);

/* 重建 canvas 缓冲到新尺寸（分辨率切换用）。 */
void ui_canvas_resize(int width, int height);

/* 将一幅 BGR 格式的 cv::Mat 贴到 canvas（自动缩放至 canvas 尺寸）。
 * 注意：必须在 UI 线程调用。 */
void ui_canvas_set_bgr(const cv::Mat &bgr);
