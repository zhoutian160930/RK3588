#pragma once
#include <string>
#include <vector>

namespace ui_log {

/* spdlog 自定义 sink 调用(任意线程)：按级别把一行推入对应缓冲 */
void push_info(const std::string &line);
void push_dbgerr(const std::string &line);

/* UI 线程调用：取走自上次以来新增的行并清空缓冲 */
std::vector<std::string> take_info();
std::vector<std::string> take_dbgerr();
}
