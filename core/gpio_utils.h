#pragma once

/* 将 TCA6424 引脚号(Pxx)转换为 sysfs GPIO 编号。
 * 自动从 /sys/kernel/debug/gpio 读取 i2c 基号(base=485)。
 * 公式: P00-P07 → base+offset, P10-P17 → base+offset-2, P20-P27 → base+offset-4 */
int gpio_calc_pin(int tca_offset);
