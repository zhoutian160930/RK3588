#pragma once
#include <string>

namespace gpio_out {

/* VR58H3 DO 通道(0-3 → /sys/class/gpio/gpiof_out0~3)。
 * 注意: 该板 DO 逻辑反相 —— 写"0"=高电平, 写"1"=低电平。 */
bool init(int ch);
void set_qualified(bool ok);
void shutdown();

/* 返回最后一次 set_qualified 的物理电平(0=low, 1=high) */
int last_output_level();
}
