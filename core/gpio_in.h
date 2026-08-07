#pragma once
#include <atomic>

namespace gpio_in {

bool init(int tca_offset);
int read();
void shutdown();

/* 外部暂停控制：P24=HIGH→暂停, LOW→恢复 */
bool is_system_paused();
void set_paused(bool v);
}
