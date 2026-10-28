/*
 * 客户端网络：连接调试服务端的 WebSocket（协议见 src/debug/viewer.h），
 * 后台线程收消息放进队列，主线程每帧取出来处理；发命令是文本帧。
 */
#ifndef MCC_CLIENT_NET_H
#define MCC_CLIENT_NET_H

#include <stdint.h>
#include <string>
#include <vector>

namespace mcc::client {

using Message = std::vector<uint8_t>;

bool net_connect(const char *host, int port);   /* 阻塞直到握手完成或失败 */
void net_connect_async(const char *host, int port); /* 后台连，用 net_connecting 看是否连完 */
bool net_connecting();
void net_close();
void net_shutdown();                             /* 退出时：等后台连接结束再断开 */
bool net_connected();
void net_send(const std::string &text);
/* 取出收到的全部消息（按到达顺序） */
void net_poll(std::vector<Message> &out);
size_t net_bytes_received();                     /* 累计收到的字节 */

} // namespace mcc::client

#endif
