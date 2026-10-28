/*
 * 简易测试客户端的服务端：HTTP 发网页，WebSocket 推送地形、结构形状与位姿，接收召唤/清空命令。
 * 不兼容原版协议，只为在浏览器里直接看到服务端的物理。
 *
 * 线程：网络收发在自己的线程；地形快照投递成区块任务、位姿在区域线程的推进后回调里写入，
 * 都只追加到各连接的发送缓冲（加锁），不直接碰区域数据之外的东西。
 *
 * 推送消息（二进制，小端）：
 *   1 地形区块  i32 cx, i32 cz, u32 n, n × { u8 lx, u8 lz, i16 y, u8 r, g, b, u8 标志 }
 *              标志：低 6 位 = 露在外面的面（-x +x -y +y -z +z），第 6 位 = 流体
 *   2 结构形状  i32 id, u32 n, n × { i16 x, y, z, u8 r, g, b }   （相对地块原点）
 *   3 位姿批次  f64 服务端毫秒, u32 n, n × { i32 id, f64 px, py, pz, f32 qx, qy, qz, qw, f32 质心 x, y, z }
 *   4 移除结构  i32 id
 *   5 推进统计  f32 最低 TPS, f32 平均 TPS, f32 平均每刻毫秒, f32 最慢一刻毫秒, u32 区域数, u32 醒着的结构, u32 结构总数
 *              （每秒一次；平均与最慢都按这一秒内所有区域的每一刻实际耗时算）
 * 命令（文本）：spawn <x> <z> <数量> | clear | push <id> <x> <y> <z> <vx> <vy> <vz> | explode <x> <y> <z> <半径> <威力>
 *   push：在世界点 (x,y,z) 给结构一个冲量，使其速度改变 (vx,vy,vz) 格/秒（按质量折算）
 *   explode：半径内的结构获得向外的速度变化，中心处为"威力"格/秒，按到结构表面的距离平方衰减
 */
#ifndef MCC_VIEWER_H
#define MCC_VIEWER_H

#include "../world/level.h"

typedef struct mcc_viewer mcc_viewer;

/* 召唤命令回调（网络线程上调用） */
typedef void (*mcc_viewer_spawn_fn)(void *ctx, double x, double z, int count);

mcc_viewer *mcc_viewer_start(mcc_level *lv, int port, const char *web_dir, mcc_viewer_spawn_fn spawn, void *ctx);
void mcc_viewer_stop(mcc_viewer *v);
/* 登记已加载的地形区块（新客户端连接时据此推送地形） */
void mcc_viewer_add_chunk(mcc_viewer *v, int32_t cx, int32_t cz);
/* 世界层的推进后回调：推送本区域结构的形状与位姿，处理清空请求 */
void mcc_viewer_post_tick(void *viewer, mcc_region *r, mcc_region_world *w);
/* 推进物理之前调用：物理枪拖动结构 */
void mcc_viewer_pre_tick(void *viewer, mcc_region *r, mcc_region_world *w);

#endif
