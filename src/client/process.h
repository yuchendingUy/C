/*
 * 自动启动本机服务端：客户端同目录下的 mcc_debug，客户端退出时关掉。
 * 单独一个文件，因为 windows.h 和 raylib 的名字冲突。
 */
#ifndef MCC_CLIENT_PROCESS_H
#define MCC_CLIENT_PROCESS_H

namespace mcc::client {

bool start_server(const char *dir, int port);   /* dir 以路径分隔符结尾 */
void stop_server();

} // namespace mcc::client

#endif
