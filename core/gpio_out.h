#pragma once
#include <string>

namespace gpio_out {

/* 初始化：通过 sysfs 导出 GPIO 引脚并设为输出(默认低电平/满足)。
 * gpio_pin = sysfs GPIO 编号(如 P17=500)。失败仅告警不阻塞。 */
bool init(int gpio_pin);

/* 设置输出电平：ok=true(满足)→低电平(0)，false(不满足)→高电平(1)。 */
void set_qualified(bool ok);

/* 关闭：恢复低电平(安全状态)。 */
void shutdown();
}
