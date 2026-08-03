#include "ui_app.h"

#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "frame_bus.h"
#include "image_process.h"
#include "mobilesam/mobilesam_pool.h"
#include "rknn_pool.h"
#include "ui_canvas.h"
#include "ui_filebrowser.h"
#include "ui_main.h"

namespace fs = std::filesystem;

namespace {
/* ---- 布局常量 ---- */
constexpr int WIN_W = 1280, WIN_H = 720;
constexpr int PANEL_W = 320;
constexpr int LEFT_W = WIN_W - PANEL_W;

/* ---- 运行期状态 ---- */
AppConfig g_cfg;
std::string g_input_path = "/home/ubuntu/lvgl/img_test";
std::string g_yolo_path = "/home/ubuntu/lvgl/yolomodel/藏药.rknn";
std::string g_label_path = "/home/ubuntu/lvgl/yolomodel/classes.txt";
std::string g_sam_enc_path;
std::string g_sam_dec_path;
bool g_use_sam = false;
int g_yolo_threads = 3;
int g_res_choice = 0; /* 0=720p, 1=1080p */

FrameBus g_bus;
std::thread g_worker;
std::atomic<bool> g_stop{false};
enum UiState { ST_IDLE = 0, ST_RUNNING = 1, ST_DONE = 2, ST_STOPPED = 3 };
std::atomic<int> g_state{ST_IDLE};
std::atomic<int> g_proc_count{0};

/* ---- 控件句柄 ---- */
lv_obj_t *g_canvas;
lv_obj_t *g_input_label, *g_yolo_label, *g_label_label;
lv_obj_t *g_samenc_label, *g_samdec_label;
lv_obj_t *g_samenc_row, *g_samdec_row;
lv_obj_t *g_status_label;
lv_obj_t *g_start_btn, *g_stop_btn;
lv_obj_t *g_res_label;
lv_obj_t *g_quit_btn;
lv_obj_t *g_spin;

std::string base_name(const std::string &p) {
  size_t s = p.find_last_of('/');
  return (s == std::string::npos) ? p : p.substr(s + 1);
}

void update_status_text() {
  const char *st = "待机";
  switch (g_state.load()) {
    case ST_RUNNING: st = "运行中"; break;
    case ST_DONE: st = "完成"; break;
    case ST_STOPPED: st = "已停止"; break;
    default: st = "待机"; break;
  }
  char buf[128];
  snprintf(buf, sizeof(buf), "状态: %s  | 已处理: %d", st, g_proc_count.load());
  lv_label_set_text(g_status_label, buf);
}

/* ---- 文件浏览器回调 ---- */
void on_fb_input(const char *path, void *) {
  g_input_path = path;
  lv_label_set_text(g_input_label, base_name(path).c_str());
}
void on_fb_yolo(const char *path, void *) {
  g_yolo_path = path;
  lv_label_set_text(g_yolo_label, base_name(path).c_str());
}
void on_fb_label(const char *path, void *) {
  g_label_path = path;
  lv_label_set_text(g_label_label, base_name(path).c_str());
}
void on_fb_samenc(const char *path, void *) {
  g_sam_enc_path = path;
  lv_label_set_text(g_samenc_label, base_name(path).c_str());
}
void on_fb_samdec(const char *path, void *) {
  g_sam_dec_path = path;
  lv_label_set_text(g_samdec_label, base_name(path).c_str());
}

void open_input_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl", ".jpg,.jpeg,.png,.bmp,.mp4,.avi,.mkv", 1,
                      on_fb_input, NULL);
}
void open_yolo_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl/yolomodel", ".rknn", 0, on_fb_yolo, NULL);
}
void open_label_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl/yolomodel", ".txt", 0, on_fb_label, NULL);
}
void open_samenc_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl", ".rknn", 0, on_fb_samenc, NULL);
}
void open_samdec_browser(lv_event_t *) {
  ui_filebrowser_open("/home/ubuntu/lvgl", ".rknn", 0, on_fb_samdec, NULL);
}

/* ---- 推理工作线程 ---- */
void worker_fn() {
  g_state = ST_RUNNING;
  g_proc_count = 0;
  g_stop = false;
  AppConfig cfg = g_cfg;

  RknnPool yolo(cfg.yolo_path, cfg.yolo_threads, cfg.label_path);
  std::unique_ptr<MobileSamPool> sam;
  if (cfg.use_sam && !cfg.sam_enc_path.empty() && !cfg.sam_dec_path.empty()) {
    sam = std::make_unique<MobileSamPool>(cfg.sam_enc_path, cfg.sam_dec_path,
                                          cfg.sam_threads);
    if (sam->Init() != 0) sam.reset();
  }

  /* 先判目录，再判图片，最后判视频；避免对目录做无意义的 imread */
  bool is_dir = fs::is_directory(cfg.input_path);
  bool is_image = false;
  cv::Mat img0;
  if (!is_dir) {
    img0 = cv::imread(cfg.input_path);
    is_image = !img0.empty();
  }
  cv::VideoCapture cap;
  bool is_video = false;
  if (!is_dir && !is_image) {
    cap.open(cfg.input_path);
    is_video = cap.isOpened();
  }
  if (!is_dir && !is_image && !is_video) {
    fprintf(stderr, "[worker] 无效输入(非目录/图片/视频): %s\n",
            cfg.input_path.c_str());
    g_state = ST_STOPPED;
    g_bus.set_done();
    return;
  }

  int submitted = 0;
  auto submit = [&](std::shared_ptr<cv::Mat> src) {
    ImageProcess ip(src->cols, src->rows, 640);
    yolo.AddInferenceTask(src, ip, "");
    submitted++;
  };
  auto drain = [&] {
    YoloResult r = yolo.GetImageResultFromQueue();
    if (!r.img) return;
    cv::Mat out = r.img->clone();
    ImageProcess ip(out.cols, out.rows, 640);
    ip.ImagePostProcess(out, r.results);
    g_bus.push(out);
    if (sam) {
      std::vector<mobilesam_box> boxes;
      for (int i = 0; i < r.results.count; i++) {
        auto &d = r.results.results[i];
        boxes.push_back({d.box.left, d.box.top, d.box.right, d.box.bottom});
      }
      char nm[300];
      snprintf(nm, sizeof(nm), "%s/%05d_sam.jpg", cfg.out_dir.c_str(),
               g_proc_count.load());
      sam->AddInferenceTask(*r.img, nm, boxes);
    }
    g_proc_count++;
  };
  auto stopped = [&] { return g_stop.load(); };

  if (is_image) {
    submit(std::make_shared<cv::Mat>(img0.clone()));
    while (g_proc_count.load() < submitted && !stopped()) {
      drain();
      usleep(2000);
    }
  } else if (is_dir) {
    std::vector<fs::path> files;
    for (auto &p : fs::directory_iterator(cfg.input_path)) {
      if (!p.is_regular_file()) continue;
      auto ext = p.path().extension().string();
      for (auto &c : ext) c = (char)std::tolower(c);
      if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
        files.push_back(p.path());
    }
    std::sort(files.begin(), files.end());
    for (auto &p : files) {
      if (stopped()) break;
      cv::Mat im = cv::imread(p.string());
      if (im.empty()) continue;
      submit(std::make_shared<cv::Mat>(im));
      while (yolo.GetTasksSize() > cfg.yolo_threads * 2) {
        drain();
        if (stopped()) break;
        usleep(5000);
      }
    }
    while (g_proc_count.load() < submitted && !stopped()) {
      drain();
      usleep(2000);
    }
  } else {
    bool eof = false;
    while (!stopped()) {
      if (!eof) {
        cv::Mat frame;
        cap >> frame;
        if (frame.empty())
          eof = true;
        else
          submit(std::make_shared<cv::Mat>(frame.clone()));
      }
      drain();
      if (eof && yolo.GetTasksSize() == 0 && g_proc_count.load() >= submitted)
        break;
      if (yolo.GetTasksSize() > cfg.yolo_threads * 2) usleep(5000);
      usleep(2000);
    }
  }

  g_bus.set_done();
  g_state = stopped() ? ST_STOPPED : ST_DONE;
}

void apply_resolution() {
  int cw, ch;
  if (g_res_choice == 1) {
    cw = 1920;
    ch = 1080;
  } else {
    cw = 1280;
    ch = 720;
  }
  ui_canvas_resize(cw, ch);
  int zoom = std::min(LEFT_W * 256 / cw, WIN_H * 256 / ch);
  lv_img_set_zoom(g_canvas, zoom);
  lv_obj_center(g_canvas);
}

void on_start(lv_event_t *) {
  if (g_state == ST_RUNNING) return;
  if (g_yolo_path.empty() || g_label_path.empty() || g_input_path.empty()) {
    lv_label_set_text(g_status_label, "请先选择 输入/YOLO模型/标签");
    return;
  }
  /* 采集 UI 参数 */
  g_cfg.input_path = g_input_path;
  g_cfg.yolo_path = g_yolo_path;
  g_cfg.label_path = g_label_path;
  g_cfg.sam_enc_path = g_sam_enc_path;
  g_cfg.sam_dec_path = g_sam_dec_path;
  g_cfg.use_sam = g_use_sam;
  g_cfg.yolo_threads = lv_spinbox_get_value(g_spin);
  g_cfg.out_dir = "outputs";
  fs::create_directories(g_cfg.out_dir);

  apply_resolution();
  g_bus.reset();
  if (g_worker.joinable()) g_worker.join();
  g_state = ST_RUNNING;
  update_status_text();
  lv_obj_add_state(g_start_btn, LV_STATE_DISABLED);
  lv_obj_clear_state(g_stop_btn, LV_STATE_DISABLED);
  g_worker = std::thread(worker_fn);
}

void on_stop(lv_event_t *) {
  if (g_state != ST_RUNNING) return;
  g_stop = true;
  if (g_worker.joinable()) g_worker.join();
  g_state = ST_STOPPED;
  update_status_text();
  lv_obj_clear_state(g_start_btn, LV_STATE_DISABLED);
  lv_obj_add_state(g_stop_btn, LV_STATE_DISABLED);
}

void on_res_toggle(lv_event_t *) {
  g_res_choice ^= 1;
  lv_label_set_text(g_res_label, g_res_choice ? "1080p" : "720p");
  if (g_state != ST_RUNNING) apply_resolution();
}

void on_sam_toggle(lv_event_t *e) {
  lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
  g_use_sam = lv_obj_has_state(sw, LV_STATE_CHECKED);
  if (g_use_sam) {
    lv_obj_clear_flag(g_samenc_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g_samdec_row, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(g_samenc_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_samdec_row, LV_OBJ_FLAG_HIDDEN);
  }
}

void on_quit(lv_event_t *) {
  g_stop = true;
  if (g_worker.joinable()) g_worker.join();
  exit(0);
}

/* 创建一行：标题 + 值标签 + 选择按钮，返回行容器。 */
lv_obj_t *add_row(lv_obj_t *parent, const char *title, lv_obj_t **value_label,
                  lv_event_cb_t pick_cb, const char *pick_txt) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_set_size(row, PANEL_W - 20, 56);
  lv_obj_set_style_pad_all(row, 2, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = lv_label_create(row);
  lv_label_set_text(t, title);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_t *v = lv_label_create(row);
  lv_label_set_text(v, "(未选)");
  lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_width(v, PANEL_W - 90);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  if (value_label) *value_label = v;
  if (pick_cb) {
    lv_obj_t *b = lv_btn_create(row);
    lv_obj_set_size(b, 56, 30);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, pick_txt);
    lv_obj_center(bl);
    lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, NULL);
  }
  return row;
}

}  // namespace

int run_ui_mode(const AppConfig &cfg) {
  if (ui_init(WIN_W, WIN_H) != 0) return -1;

  /* 用命令行传入的输入路径作为默认（模型/标签沿用本机可用默认值） */
  if (!cfg.input_path.empty()) g_input_path = cfg.input_path;

  /* 左侧画布区 */
  lv_obj_t *left = lv_obj_create(lv_scr_act());
  lv_obj_set_size(left, LEFT_W, WIN_H);
  lv_obj_set_style_bg_color(left, lv_color_make(20, 20, 20), 0);
  lv_obj_set_style_border_width(left, 0, 0);
  lv_obj_set_style_pad_all(left, 0, 0);
  lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
  g_canvas = ui_canvas_create(left, 1280, 720);
  lv_obj_center(g_canvas);

  /* 右侧控制面板 */
  lv_obj_t *panel = lv_obj_create(lv_scr_act());
  lv_obj_set_size(panel, PANEL_W, WIN_H);
  lv_obj_set_pos(panel, LEFT_W, 0);
  lv_obj_set_style_pad_all(panel, 8, 0);
  lv_obj_t *pt = lv_label_create(panel);
  lv_label_set_text(pt, "YOLO 推理控制台");
  lv_obj_align(pt, LV_ALIGN_TOP_MID, 0, 0);

  /* 预填当前值标签 */
  lv_obj_t *r1 = add_row(panel, "输入源(图/视频/目录)", &g_input_label,
                         open_input_browser, "选");
  lv_obj_align(r1, LV_ALIGN_TOP_MID, 0, 24);
  lv_label_set_text(g_input_label, base_name(g_input_path).c_str());

  lv_obj_t *r2 = add_row(panel, "YOLO 模型(.rknn)", &g_yolo_label,
                         open_yolo_browser, "选");
  lv_obj_align_to(r2, r1, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_label_set_text(g_yolo_label, base_name(g_yolo_path).c_str());

  lv_obj_t *r3 = add_row(panel, "标签文件(.txt)", &g_label_label,
                         open_label_browser, "选");
  lv_obj_align_to(r3, r2, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_label_set_text(g_label_label, base_name(g_label_path).c_str());

  /* SAM 开关 */
  lv_obj_t *sam_row = lv_obj_create(panel);
  lv_obj_set_size(sam_row, PANEL_W - 20, 44);
  lv_obj_set_style_pad_all(sam_row, 2, 0);
  lv_obj_set_style_border_width(sam_row, 0, 0);
  lv_obj_clear_flag(sam_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(sam_row, r3, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_t *sl = lv_label_create(sam_row);
  lv_label_set_text(sl, "SAM 分割(掩膜存文件)");
  lv_obj_align(sl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *sw = lv_switch_create(sam_row);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_state(sw, g_use_sam ? LV_STATE_CHECKED : 0);
  lv_obj_add_event_cb(sw, on_sam_toggle, LV_EVENT_VALUE_CHANGED, NULL);

  g_samenc_row = add_row(panel, "SAM encoder(.rknn)", &g_samenc_label,
                         open_samenc_browser, "选");
  lv_obj_align_to(g_samenc_row, sam_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  g_samdec_row = add_row(panel, "SAM decoder(.rknn)", &g_samdec_label,
                         open_samdec_browser, "选");
  lv_obj_align_to(g_samdec_row, g_samenc_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_add_flag(g_samenc_row, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_samdec_row, LV_OBJ_FLAG_HIDDEN);

  /* 线程 spinbox */
  lv_obj_t *thr_row = lv_obj_create(panel);
  lv_obj_set_size(thr_row, PANEL_W - 20, 44);
  lv_obj_set_style_pad_all(thr_row, 2, 0);
  lv_obj_set_style_border_width(thr_row, 0, 0);
  lv_obj_clear_flag(thr_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(thr_row, g_samdec_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_t *tl = lv_label_create(thr_row);
  lv_label_set_text(tl, "YOLO 线程");
  lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *spin = lv_spinbox_create(thr_row);
  lv_spinbox_set_range(spin, 1, 6);
  lv_spinbox_set_digit_format(spin, 1, 0);
  lv_spinbox_set_value(spin, g_yolo_threads);
  lv_obj_align(spin, LV_ALIGN_RIGHT_MID, 0, 0);
  g_spin = spin;

  /* 分辨率按钮 */
  lv_obj_t *res_row = lv_obj_create(panel);
  lv_obj_set_size(res_row, PANEL_W - 20, 44);
  lv_obj_set_style_pad_all(res_row, 2, 0);
  lv_obj_set_style_border_width(res_row, 0, 0);
  lv_obj_clear_flag(res_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(res_row, thr_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
  lv_obj_t *rl = lv_label_create(res_row);
  lv_label_set_text(rl, "分辨率");
  lv_obj_align(rl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *res_btn = lv_btn_create(res_row);
  lv_obj_set_size(res_btn, 90, 32);
  lv_obj_align(res_btn, LV_ALIGN_RIGHT_MID, 0, 0);
  g_res_label = lv_label_create(res_btn);
  lv_label_set_text(g_res_label, "720p");
  lv_obj_center(g_res_label);
  lv_obj_add_event_cb(res_btn, on_res_toggle, LV_EVENT_CLICKED, NULL);

  /* 开始 / 停止 */
  lv_obj_t *btn_row = lv_obj_create(panel);
  lv_obj_set_size(btn_row, PANEL_W - 20, 48);
  lv_obj_set_style_pad_all(btn_row, 2, 0);
  lv_obj_set_style_border_width(btn_row, 0, 0);
  lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align_to(btn_row, res_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
  g_start_btn = lv_btn_create(btn_row);
  lv_obj_set_size(g_start_btn, 110, 40);
  lv_obj_align(g_start_btn, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *slb = lv_label_create(g_start_btn);
  lv_label_set_text(slb, "开始");
  lv_obj_center(slb);
  lv_obj_add_event_cb(g_start_btn, on_start, LV_EVENT_CLICKED, NULL);
  g_stop_btn = lv_btn_create(btn_row);
  lv_obj_set_size(g_stop_btn, 110, 40);
  lv_obj_align(g_stop_btn, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_t *spb = lv_label_create(g_stop_btn);
  lv_label_set_text(spb, "停止");
  lv_obj_center(spb);
  lv_obj_add_state(g_stop_btn, LV_STATE_DISABLED);
  lv_obj_add_event_cb(g_stop_btn, on_stop, LV_EVENT_CLICKED, NULL);

  /* 状态 */
  g_status_label = lv_label_create(panel);
  lv_obj_align_to(g_status_label, btn_row, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
  update_status_text();

  /* 退出 */
  g_quit_btn = lv_btn_create(panel);
  lv_obj_set_size(g_quit_btn, PANEL_W - 20, 36);
  lv_obj_align(g_quit_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_t *ql = lv_label_create(g_quit_btn);
  lv_label_set_text(ql, "退出");
  lv_obj_center(ql);
  lv_obj_add_event_cb(g_quit_btn, on_quit, LV_EVENT_CLICKED, NULL);

  apply_resolution();

  /* 主循环：永不因推理完成退出，只在 退出按钮 时结束 */
  while (true) {
    ui_tick();
    cv::Mat frame;
    if (g_bus.pop(frame)) ui_canvas_set_bgr(frame);
    if (g_state.load() == ST_RUNNING || g_state.load() == ST_DONE ||
        g_state.load() == ST_STOPPED)
      update_status_text();
    usleep(5000);
  }
  return 0;
}
