#pragma once

namespace gpio_in {

/* 初始化：通过 sysfs 导出并设为输入。失败仅告警。 */
bool init(int gpio_pin);

/* 读回当前电平：0(低)=满足, 1(高)=不满足。失败返回 -1。 */
int read();

/* 关闭。 */
void shutdown();
}
