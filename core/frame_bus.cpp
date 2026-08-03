#include "frame_bus.h"

void FrameBus::push(const FramePayload &p) {
  std::lock_guard<std::mutex> lk(mtx_);
  latest_ = p;
  has_new_ = true;
  pending_ = true;
}

bool FrameBus::pop(FramePayload &out) {
  std::lock_guard<std::mutex> lk(mtx_);
  if (!has_new_) return false;
  out = latest_;
  has_new_ = false;
  pending_ = false;
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

bool FrameBus::is_pending() {
  std::lock_guard<std::mutex> lk(mtx_);
  return pending_;
}

void FrameBus::reset() {
  std::lock_guard<std::mutex> lk(mtx_);
  latest_ = FramePayload{};
  has_new_ = false;
  done_ = false;
  pending_ = false;
}
