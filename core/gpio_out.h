#pragma once
#include <string>

namespace gpio_out {

bool init(int gpio_pin);
void set_qualified(bool ok);
void shutdown();

/* 返回最后一次 set_qualified 写的值(0=low, 1=high)，供输入回环验证 */
int last_output_val();
}
