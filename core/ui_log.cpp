#include "ui_log.h"
#include <mutex>

namespace {
constexpr size_t CAP = 200;
std::vector<std::string> g_info;
std::vector<std::string> g_dbgerr;
std::mutex g_mtx;
}

namespace ui_log {

void push_info(const std::string &line) {
  std::lock_guard<std::mutex> lk(g_mtx);
  g_info.push_back(line);
  if (g_info.size() > CAP) g_info.erase(g_info.begin());
}

void push_dbgerr(const std::string &line) {
  std::lock_guard<std::mutex> lk(g_mtx);
  g_dbgerr.push_back(line);
  if (g_dbgerr.size() > CAP) g_dbgerr.erase(g_dbgerr.begin());
}

std::vector<std::string> take_info() {
  std::lock_guard<std::mutex> lk(g_mtx);
  std::vector<std::string> out;
  out.swap(g_info);
  return out;
}

std::vector<std::string> take_dbgerr() {
  std::lock_guard<std::mutex> lk(g_mtx);
  std::vector<std::string> out;
  out.swap(g_dbgerr);
  return out;
}

}
