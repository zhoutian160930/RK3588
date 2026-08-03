#include "frame_bus.h"

void FrameBus::push(const cv::Mat &frame) {
  std::lock_guard<std::mutex> lk(mtx_);
  frame.copyTo(latest_);
  has_new_ = true;
}

bool FrameBus::pop(cv::Mat &out) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (!has_new_) return false;
  latest_.copyTo(out);
  has_new_ = false;
  return true;
}

void FrameBus::set_done() {
  std::lock_guard<std::mutex> lk(mtx_);
  done_ = true;
}

bool FrameBus::is_done_and_drained() {
  std::lock_guard<std::mutex> lk(mtx_);
  return done_ && !has_new_;
}

void FrameBus::reset() {
  std::lock_guard<std::mutex> lk(mtx_);
  latest_.release();
  has_new_ = false;
  done_ = false;
}
