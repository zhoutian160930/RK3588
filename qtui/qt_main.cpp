/* Qt 版入口:初始化顺序与旧 main 一致(config→logger→CAN→GPIO→GPIO轮询) */
#include <QApplication>

#include <spdlog/spdlog.h>

#include <thread>
#include <unistd.h>

#include "can_bus.h"
#include "config.h"
#include "gpio_in.h"
#include "gpio_out.h"
#include "logger.h"
#include "mainwindow.h"

int main(int argc, char *argv[]) {
  config::init(config::exe_dir() + "/../config", "parameters.json");
  logger::init(config::g.log_root);
  SPDLOG_INFO("==== yolov8_sam_demo (Qt UI) ====");

  if (config::g.can_enabled)
    can_bus::init(config::g.can_send_if, config::g.can_recv_if,
                  config::g.can_id);
  if (config::g.gpio_enabled) gpio_out::init(config::g.gpio_out_pin);
  if (config::g.gpio_input_enabled) {
    gpio_in::init(config::g.gpio_input_pin);
    std::thread([] {
      while (true) {
        gpio_in::set_paused(gpio_in::read() == 1);
        usleep(100000);
      }
    }).detach();
  }

  QApplication app(argc, argv);
  MainWindow w;
  /* 最大化启动(保留系统标题栏:右上角 最小化/缩放/关闭 三按钮;
   * 缩放=最大化↔窗口切换,关闭=走 closeEvent 清理退出) */
  w.showMaximized();
  return app.exec();
}
