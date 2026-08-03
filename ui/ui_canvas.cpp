#include "ui_canvas.h"

#include <stdlib.h>
#include <string.h>

#include <opencv2/imgproc.hpp>

static lv_obj_t *s_canvas = NULL;
static lv_color_t *s_buf = NULL;
static int s_w = 0;
static int s_h = 0;

lv_obj_t *ui_canvas_create(lv_obj_t *parent, int width, int height) {
  s_w = width;
  s_h = height;
  if (!s_buf) s_buf = (lv_color_t *)malloc((size_t)width * height * sizeof(lv_color_t));
  s_canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(s_canvas, s_buf, width, height, LV_IMG_CF_TRUE_COLOR);
  lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
  return s_canvas;
}

void ui_canvas_resize(int width, int height) {
  s_w = width;
  s_h = height;
  if (s_buf) free(s_buf);
  s_buf = (lv_color_t *)malloc((size_t)width * height * sizeof(lv_color_t));
  if (s_canvas) {
    lv_canvas_set_buffer(s_canvas, s_buf, width, height, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
  }
}

void ui_canvas_set_bgr(const cv::Mat &bgr) {
  if (!s_canvas || bgr.empty()) return;
  cv::Mat resized;
  if (bgr.cols != s_w || bgr.rows != s_h) {
    cv::resize(bgr, resized, cv::Size(s_w, s_h));
  } else {
    resized = bgr;
  }
  /* LV_COLOR_DEPTH=32 时 lv_color_t 内存布局为 B,G,R,A，与 OpenCV BGRA 一致 */
  cv::Mat bgra;
  cv::cvtColor(resized, bgra, cv::COLOR_BGR2BGRA);
  memcpy(s_buf, bgra.data, (size_t)s_w * s_h * sizeof(lv_color_t));
  lv_obj_invalidate(s_canvas);
}
