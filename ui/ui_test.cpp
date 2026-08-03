/* Phase 1 验证：独立程序，验证 LVGL+SDL+canvas 显示通路（ssh -X 可见窗口）。
 * 用法: ./ui_test [图片路径]
 */
#include <unistd.h>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include "ui_canvas.h"
#include "ui_main.h"

int main(int argc, char **argv) {
  const int w = 1280, h = 720;
  if (ui_init(w, h) != 0) {
    std::cerr << "ui_init failed\n";
    return -1;
  }

  ui_canvas_create(lv_scr_act(), w, h);

  cv::Mat img;
  if (argc >= 2) {
    img = cv::imread(argv[1]);
  }
  if (img.empty()) {
    img =
        cv::imread("/home/ubuntu/lvgl/img_test/00000958_64415.jpg");
  }
  if (!img.empty()) {
    ui_canvas_set_bgr(img);
  }

  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "LVGL + SDL OK   (click to test mouse)");
  lv_obj_set_style_text_color(label, lv_color_white(), 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

  std::cout << "UI running. Close the SDL window or Ctrl-C to exit.\n";
  while (true) {
    ui_tick();
    usleep(5000);
  }
  return 0;
}
