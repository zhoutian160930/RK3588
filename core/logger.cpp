#include "logger.h"

#include <ctime>
#include <filesystem>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

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

  auto lg = std::make_shared<spdlog::logger>(
      "app", spdlog::sinks_init_list{console_sink, file_sink});
  lg->set_level(spdlog::level::debug);
  lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  lg->flush_on(spdlog::level::debug);
  spdlog::set_default_logger(lg);
  spdlog::info("spdlog initialized, log file: {}", path);
}

}  // namespace logger
