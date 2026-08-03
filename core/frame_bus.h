#pragma once
#include <opencv2/core.hpp>
#include <mutex>

/* 线程安全的"最新帧"信箱：推理工作线程 push，UI 线程 pop。
 * 仅保留最新一帧，避免显示积压（实时显示场景下丢弃中间帧）。 */
class FrameBus {
 public:
  /* 推理线程调用：提交一帧合成好的 BGR 图像。 */
  void push(const cv::Mat &frame);

  /* UI 线程调用：取出最新帧。有新帧返回 true。 */
  bool pop(cv::Mat &out);

  /* 推理线程完成时调用。 */
  void set_done();

  /* UI 线程判断管线是否已结束（且无新帧）。 */
  bool is_done_and_drained();

  /* 重置信箱状态（新一轮推理前调用）。 */
  void reset();

 private:
  cv::Mat latest_;
  bool has_new_ = false;
  bool done_ = false;
  std::mutex mtx_;
};
