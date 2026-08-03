#include "ui_main.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "lvgl.h"
#include "sdl/sdl.h"
#include "src/extra/libs/freetype/lv_freetype.h"

/* 中文字体路径：优先用仓库内，否则用系统 Noto Sans CJK。
 * 量产时建议把字体文件放进仓库 fonts/ 目录。 */
#define CN_FONT_REPO "/home/ubuntu/lvgl/yolov8/fonts/NotoSansCJK-Regular.ttc"
#define CN_FONT_SYS "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"

static lv_font_t *s_cn_font = NULL;
static void load_chinese_font(void) {
  const char *path = (access(CN_FONT_REPO, R_OK) == 0) ? CN_FONT_REPO : CN_FONT_SYS;
  lv_freetype_init(4, 4, 1 * 1024 * 1024);
  lv_ft_info_t info;
  memset(&info, 0, sizeof(info));
  info.name = path;
  info.weight = 22;
  info.style = FT_FONT_STYLE_NORMAL;
  if (lv_ft_font_init(&info)) {
    s_cn_font = info.font;
    lv_obj_set_style_text_font(lv_scr_act(), s_cn_font, 0);
  } else {
    fprintf(stderr, "[ui] load Chinese font failed: %s\n", path);
  }
}

static lv_disp_drv_t s_disp_drv;
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_buf1 = NULL;

uint32_t custom_tick_get(void) {
  static uint64_t start_ms = 0;
  struct timeval tv;
  if (start_ms == 0) {
    gettimeofday(&tv, NULL);
    start_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  }
  gettimeofday(&tv, NULL);
  uint64_t now_ms = (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  return (uint32_t)(now_ms - start_ms);
}

int ui_init(int width, int height) {
  lv_init();
  sdl_init();

  /* 显示绘制缓冲：半屏大小，足够流畅且省内存 */
  size_t n = (size_t)width * height / 2;
  s_buf1 = (lv_color_t *)malloc(n * sizeof(lv_color_t));
  if (!s_buf1) return -1;
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, NULL, n);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.draw_buf = &s_draw_buf;
  s_disp_drv.flush_cb = sdl_display_flush;
  s_disp_drv.hor_res = width;
  s_disp_drv.ver_res = height;
  lv_disp_drv_register(&s_disp_drv);

  /* 显示注册后 lv_scr_act() 才有效，此时加载中文字体并设为屏幕默认 */
  load_chinese_font();

  /* 鼠标指针输入 */
  static lv_indev_drv_t mouse_drv;
  lv_indev_drv_init(&mouse_drv);
  mouse_drv.type = LV_INDEV_TYPE_POINTER;
  mouse_drv.read_cb = sdl_mouse_read;
  lv_indev_t *mouse = lv_indev_drv_register(&mouse_drv);

  LV_IMG_DECLARE(mouse_cursor_icon);
  lv_obj_t *cursor = lv_img_create(lv_scr_act());
  lv_img_set_src(cursor, &mouse_cursor_icon);
  lv_indev_set_cursor(mouse, cursor);

  /* 键盘输入 */
  static lv_indev_drv_t kb_drv;
  lv_indev_drv_init(&kb_drv);
  kb_drv.type = LV_INDEV_TYPE_KEYPAD;
  kb_drv.read_cb = sdl_keyboard_read;
  lv_indev_drv_register(&kb_drv);

  return 0;
}

void ui_tick(void) { lv_timer_handler(); }
