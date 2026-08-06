/* GPIO 输出模块 —— 基于 sysfs (/sys/class/gpio/)，与 test.cpp 验证通过的方式一致。
 * 用于 P17(sysfs pin 500)：满足→低电平，不满足→高电平。
 */
#include "gpio_out.h"
#include "gpio_utils.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <unistd.h>

namespace gpio_out {

static int g_pin = -1;
static int g_tca_offset = -1;
static int g_last_val = 0;
static std::string g_value_path;

static bool sysfs_write(const std::string &path, const std::string &val) {
  int fd = open(path.c_str(), O_WRONLY);
  if (fd < 0) return false;
  write(fd, val.c_str(), val.size());
  close(fd);
  return true;
}

bool init(int tca_offset) {
  g_tca_offset = tca_offset;
  g_pin = gpio_calc_pin(tca_offset);
  std::string base = "/sys/class/gpio/gpio" + std::to_string(g_pin);
  g_value_path = base + "/value";

  if (access(base.c_str(), F_OK) != 0) {
    if (!sysfs_write("/sys/class/gpio/export", std::to_string(g_pin))) {
      SPDLOG_WARN("GPIO: 导出 P{} (sysfs {}) 失败(需 root)", tca_offset, g_pin);
      g_pin = -1;
      return false;
    }
    usleep(100000);
  }

  sysfs_write(base + "/direction", "out");
  sysfs_write(g_value_path, "0");
  g_last_val = 0;

  SPDLOG_INFO("GPIO: P{} (sysfs {}) 输出就绪, 初始 LOW", tca_offset, g_pin);
  std::atexit(+[] { shutdown(); });
  return true;
}

void set_qualified(bool ok) {
  if (g_pin < 0) return;
  const char *val = ok ? "0" : "1";
  if (!sysfs_write(g_value_path, val)) {
    SPDLOG_ERROR("[GPIO-out] 写 pin {} 失败: {}", g_pin, strerror(errno));
    return;
  }
  g_last_val = ok ? 0 : 1;
  SPDLOG_INFO("[GPIO-out] P{}={} ({})", g_tca_offset, ok ? "LOW" : "HIGH",
              ok ? "满足" : "不满足");
}

void shutdown() {
  if (g_pin >= 0) {
    sysfs_write(g_value_path, "0");
    g_last_val = 0;
    SPDLOG_INFO("GPIO: P{} 已恢复 LOW", g_tca_offset);
    g_pin = -1;
  }
}

int last_output_val() { return g_last_val; }

}  // namespace gpio_out
