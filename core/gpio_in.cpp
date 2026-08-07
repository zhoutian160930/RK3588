#include "gpio_in.h"
#include "gpio_utils.h"

#include <fcntl.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <unistd.h>

namespace gpio_in {

static int g_pin = -1;
static int g_tca_offset = -1;
static std::string g_value_path;
static std::atomic<bool> g_paused{false};

static bool sysfs_write(const std::string &p, const std::string &v) {
  int fd = open(p.c_str(), O_WRONLY);
  if (fd < 0) return false;
  write(fd, v.c_str(), v.size());
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
      SPDLOG_WARN("GPIO-in: 导出 P{} (sysfs {}) 失败(需 root)", tca_offset, g_pin);
      g_pin = -1;
      return false;
    }
    usleep(100000);
  }
  sysfs_write(base + "/direction", "in");
  SPDLOG_INFO("GPIO-in: P{} (sysfs {}) 输入就绪", tca_offset, g_pin);
  std::atexit(+[] { shutdown(); });
  return true;
}

int read() {
  if (g_pin < 0) return -1;
  char buf[4];
  int fd = open(g_value_path.c_str(), O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return -1;
  buf[n] = '\0';
  return (buf[0] == '1') ? 1 : 0;
}

void shutdown() {
  if (g_pin >= 0) { g_pin = -1; }
}

bool is_system_paused() { return g_paused.load(); }
void set_paused(bool v) { g_paused.store(v); }

}  // namespace gpio_in
