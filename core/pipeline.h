#pragma once
/* 推理管线(从旧 LVGL ui_app.cpp 抽出的 worker 逻辑,UI 无关)。
 * 职责:输入源(相机/图片/目录/视频) → RknnPool 推理 → 画框/画线 → 判定
 *      → CAN/GPIO 发布 → FrameBus 投递给 UI + 运行期共享状态(atomic)。
 * UI 层(Qt/LVGL)通过本头文件的状态与 FrameBus 交互,不直接接触推理栈。 */

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "app_config.h"
#include "frame_bus.h"
#include "rknn_pool.h"

namespace pipeline {

/* 运行状态机 */
enum UiState { ST_IDLE = 0, ST_RUNNING = 1, ST_DONE = 2, ST_STOPPED = 3 };

/* ---- UI ↔ worker 运行期共享状态(atomic,竖线坐标基于显示宽 CV_W 归一化前的像素) ---- */
extern std::atomic<int> g_line_left_x;    /* 左竖线 x ∈ [0, view_width] */
extern std::atomic<int> g_line_right_x;   /* 右竖线 x */
extern std::atomic<int> g_target_count;   /* 目标物料数 */
extern std::atomic<int> g_material_class; /* 物料类别 ID */
extern std::atomic<int> g_box_class;      /* 盒子类别 ID */
extern std::atomic<int> g_correct_count;  /* 累计正确(按帧) */
extern std::atomic<int> g_wrong_count;    /* 累计错误(按帧) */
extern std::atomic<int> g_proc_count;     /* 已处理帧数 */
extern std::atomic<int> g_state;          /* UiState */
extern std::atomic<bool> g_stop;          /* 停止请求 */
extern std::atomic<bool> g_capture_mode;  /* 采集图像模式 */

extern FrameBus g_bus;                        /* worker→UI 最新帧信箱 */
extern FramePayload g_last_payload;           /* 最近一帧(拖线实时重判用) */
extern bool g_has_last;

/* 视图坐标宽度(竖线像素定义域)。worker 内部换算为比例。
 * 旧版固定 820;Qt 版显示区可变,由 UI 在 init/resize 时设置。 */
extern int g_view_width;

/* 输入源配置(UI 设置;Start 时快照进 g_cfg) */
void set_input_source(const std::string &path);   /* 文件/目录路径 */
void set_use_camera(bool use);                    /* true=相机输入 */
void set_yolo(const std::string &model_path, const std::string &label_path);
void set_sam(const std::string &enc, const std::string &dec, bool use);
void set_yolo_threads(int n);

/* 模型热切换:释放常驻 RknnPool(须在 STOPPED/IDLE 且 worker 已 join 时调用)。
 * 返回 false=运行中不允许。下次 start() 会以当前路径重建加载。 */
bool release_yolo_pool();

/* 启动 worker(内部快照当前输入配置)。调用前须保证 worker 不可 join。
 * 返回 false=输入路径为空。 */
bool start();

/* 请求停止并 join worker。幂等。 */
void stop_and_join();

/* worker 是否 joinable(运行中) */
bool running();

}  // namespace pipeline
