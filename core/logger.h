#pragma once
#include <string>

namespace logger {
/* 初始化 spdlog：控制台 + 文件双 sink，文件写到 log_dir/<时间戳>.txt。
 * 默认 DEBUG 级别，每条 debug 即 flush（便于崩溃前已落盘）。 */
void init(const std::string &log_dir = "log");
}
