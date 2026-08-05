#pragma once
#include <string>

namespace can_bus {

/* 初始化：打开发送接口(can0)与接收接口(can1, 启动接收线程)。
 * 失败(接口未就绪)仅告警，不阻塞主流程。返回是否至少发送端可用。 */
bool init(const std::string &send_if, const std::string &recv_if, int can_id);

/* 发送推理结果：ok=true 发 0x00，false 发 0x01。同时打印 logger。
 * 发送端不可用时为 no-op。 */
void send_result(bool ok);

/* 关闭并回收接收线程(主循环退出前调用)。 */
void shutdown();
}
