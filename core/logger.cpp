#include "logger.h"

#include <ctime>
#include <filesystem>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "ui_log.h"

namespace fs = std::filesystem;

/* 自定义 sink：按级别把日志推到 UI 双缓冲(info / debug+error) */
class UiLogSink : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg &msg) override {
    std::string s(msg.payload.data(), msg.payload.size());
    if (msg.level == spdlog::level::info) {
      ui_log::push_info(s);
    } else if (msg.level == spdlog::level::debug ||
               msg.level == spdlog::level::err ||
               msg.level == spdlog::level::warn) {
      const char *tag =
          (msg.level == spdlog::level::err)     ? "[ERR] "
          : (msg.level == spdlog::level::warn)  ? "[WARN] "
                                                : "[DBG] ";
      ui_log::push_dbgerr(tag + s);
    }
  }
  void flush_() override {}
};

namespace logger {

void init(const std::string &log_dir) {
  fs::create_directories(log_dir);
  std::time_t now = std::time(nullptr);
  char ts[32];
  std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now));
  std::string path = log_dir + "/" + ts + ".txt";

  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_level(spdlog::level::info);
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, false);
  file_sink->set_level(spdlog::level::trace);
  auto ui_sink = std::make_shared<UiLogSink>();
  ui_sink->set_level(spdlog::level::debug);

  auto lg = std::make_shared<spdlog::logger>(
      "app", spdlog::sinks_init_list{console_sink, file_sink, ui_sink});
  lg->set_level(spdlog::level::debug);
  lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  lg->flush_on(spdlog::level::debug);
  spdlog::set_default_logger(lg);
  spdlog::info("spdlog initialized, log file: {}", path);
}

}  // namespace logger
