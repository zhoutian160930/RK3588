/* GPIO 输出模块 —— 基于 sysfs (/sys/class/gpio/)，与 test.cpp 验证通过的方式一致。
 * 用于 P17(sysfs pin 500)：满足→低电平，不满足→高电平。
 */
#include "gpio_out.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <unistd.h>

namespace gpio_out {

static int g_pin = -1;
static std::string g_value_path;

/* 写字符串到 sysfs 文件 */
static bool sysfs_write(const std::string &path, const std::string &val) {
  int fd = open(path.c_str(), O_WRONLY);
  if (fd < 0) return false;
  write(fd, val.c_str(), val.size());
  close(fd);
  return true;
}

bool init(int gpio_pin) {
  g_pin = gpio_pin;
  std::string base = "/sys/class/gpio/gpio" + std::to_string(gpio_pin);
  g_value_path = base + "/value";

  /* 检查是否已导出，没有则导出 */
  if (access(base.c_str(), F_OK) != 0) {
    if (!sysfs_write("/sys/class/gpio/export", std::to_string(gpio_pin))) {
      SPDLOG_WARN("GPIO: 导出 pin {} 失败(需 root 权限)", gpio_pin);
      g_pin = -1;
      return false;
    }
    usleep(100000); /* 等待 sysfs 创建节点 */
  }

  /* 设为输出 + 初始低电平(满足/安全) */
  sysfs_write(base + "/direction", "out");
  sysfs_write(g_value_path, "0");

  SPDLOG_INFO("GPIO: pin {} (P17) 输出就绪, 初始 LOW", gpio_pin);
  std::atexit(+[] { shutdown(); });
  return true;
}

void set_qualified(bool ok) {
  if (g_pin < 0) return;
  const char *val = ok ? "0" : "1"; /* 满足→低, 不满足→高 */
  if (!sysfs_write(g_value_path, val)) {
    SPDLOG_ERROR("[GPIO-out] 写 pin {} 失败: {}", g_pin, strerror(errno));
    return;
  }
  SPDLOG_INFO("[GPIO-out] P17={} ({})", ok ? "LOW" : "HIGH",
              ok ? "满足" : "不满足");
}

void shutdown() {
  if (g_pin >= 0) {
    sysfs_write(g_value_path, "0"); /* 恢复低电平 */
    SPDLOG_INFO("GPIO: pin {} 已恢复 LOW", g_pin);
    g_pin = -1;
  }
}

}  // namespace gpio_out
