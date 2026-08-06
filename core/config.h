#pragma once
#include <string>

/* 全局配置（JSON 持久化 + 热加载）。扁平结构，每行一个参数。 */
struct Config {
  /* A: 任务/判定参数 */
  int material_class = 0;     /* 物料类别 ID */
  int box_class = 1;          /* 盒子类别 ID */
  int target_count = 10;      /* 满料目标物料数 */
  double line_left_frac = 0.25;   /* 左竖线占图宽比例 */
  double line_right_frac = 0.75;  /* 右竖线占图宽比例 */
  /* B: 路径 */
  std::string yolo_model = "model/yolov8s.rknn";
  std::string label_path = "model/coco_80_labels_list.txt";
  std::string sam_encoder = "model/mobile_sam_encoder.rknn";
  std::string sam_decoder = "model/mobile_sam_decoder.rknn";
  std::string detImg_root = "/home/ubuntu/lvgl/output/detImg";
  std::string log_root = "/home/ubuntu/lvgl/output/log";
  int yolo_threads = 3;
  bool use_sam = false;
  int sam_threads = 3;
  std::string default_input;
  std::string fb_model_dir = "/home/ubuntu/lvgl/yolomodel";
  std::string fb_input_dir = "/home/ubuntu/lvgl";
  std::string font_path;
  int ui_width = 1280;
  int ui_height = 720;
  int panel_width = 320;
  int default_res = 0;
  /* CAN 通信 */
  bool can_enabled = true;            /* 是否启用 CAN 结果发送 */
  std::string can_send_if = "can0";   /* 发送接口 */
  std::string can_recv_if = "can1";   /* 接收接口(本地回环测试) */
  int can_id = 0x300;                 /* 标准 11-bit CAN ID */
  /* GPIO 结果输出(TCA6424, sysfs 方式)
   * pin 计算: base=485(查 /sys/kernel/debug/gpio 中 i2c 行的起始号)
   *   P00-P07: pin = base + offset
   *   P10-P17: pin = base + offset - 2
   *   P20-P27: pin = base + offset - 4
   * 例: P17=485+17-2=500, P23=485+23-4=504 */
  bool gpio_enabled = true;
  int gpio_pin = 500;                 /* P17 = 485 + 17 - 2 */
};

namespace config {

extern Config g;                  /* 全局配置实例 */
extern std::string file_path;     /* config.json 路径 */

/* 初始化：加载 config/<file>；不存在则生成默认。 */
void init(const std::string &config_dir = "config",
          const std::string &file = "config.json");

/* 把内存中的当前值写回 JSON（pretty，每行一个参数）。 */
void save();

/* 主循环周期调用：若文件被外部编辑(mtime 变)则重新加载，返回 true。 */
bool poll_hot_reload();

/* 主循环周期调用：若有脏数据且距上次保存>1s，返回 true（调用方应先 sync 再 save）。 */
bool poll_save_due();

/* 标记内存值已改、需要回写（UI 改动后调用，实际写入会防抖节流）。 */
void mark_dirty();
}
