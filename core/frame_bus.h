#pragma once
#include <opencv2/core.hpp>
#include <mutex>
#include "common.h"  // object_detect_result_list

/* 一帧完整数据：带检测框的画面 + 原始检测结果 + 原图尺寸(用于坐标换算) */
struct FramePayload {
  cv::Mat frame;                      /* 已画框的 BGR 画面(原图尺寸) */
  object_detect_result_list results{};/* 检测结果(原图坐标) */
  int orig_w = 0;
  int orig_h = 0;
};

/* 线程安全的"最新帧"信箱：推理工作线程 push，UI 线程 pop。
 * 仅保留最新一帧，避免显示积压；带 is_pending 实现"推理一帧展示一帧"。 */
class FrameBus {
 public:
  void push(const FramePayload &p);
  bool pop(FramePayload &out);
  void set_done();
  bool is_done_and_drained();
  bool is_pending();
  void reset();

 private:
  FramePayload latest_;
  bool has_new_ = false;
  bool done_ = false;
  bool pending_ = false;
  std::mutex mtx_;
};
