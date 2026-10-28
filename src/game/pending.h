/*
 * 留空接口：机械动力现在就要调用、但对应的系统还没做的地方，全部集中在这里。
 *
 * 每个接口现在什么都不做（或返回"没有"），调用处保持和原版一样的位置与时机；
 * 以后做对应系统时只改这里的实现，调用方不用动。
 * 搜索 "TODO(" 就能找全，标签：
 *   TODO(红石)      原版红石信号的读取与输出
 *   TODO(掉落物)    方块掉落物、物品实体
 *   TODO(网络)      同步给客户端（方块更新、方块实体数据、世界事件）
 *   TODO(流体)      原版流体流动（含水方块的流体刻）
 *   TODO(存档)      区块与方块实体的保存、读取
 *   TODO(玩家交互)  玩家对方块的操作（扳手、滚动数值面板、放置朝向）
 *   TODO(物理结构)  动态结构 / 火车改成原生物理结构后的接入点
 */
#ifndef MCC_GAME_PENDING_H
#define MCC_GAME_PENDING_H

#include "types.h"
#include "../world/blocks.h"

namespace mcc {

struct Space;
struct BlockEntity;

/* TODO(红石)：原版 Level.hasNeighborSignal —— 六个方向任一有红石信号输入 */
MCC_INLINE bool redstone_has_neighbor_signal(Space &, BlockPos) { return false; }
/* TODO(红石)：原版 Level.getBestNeighborSignal —— 六个方向输入信号的最大值 0~15 */
MCC_INLINE int redstone_best_neighbor_signal(Space &, BlockPos) { return 0; }
/* TODO(红石)：原版 Level.updateNeighbourForOutputSignal —— 通知比较器 */
MCC_INLINE void redstone_update_output_signal(Space &, BlockPos, mcc_block_t) {}

/* TODO(掉落物)：原版 Block.dropResources —— 按战利品表掉落物品 */
MCC_INLINE void drops_drop_resources(Space &, BlockPos, mcc_state_t, BlockEntity *) {}

/* TODO(网络)：正式的网络协议还没做。现在调试客户端（src/debug/viewer）在这里挂钩子，
 * 收方块变化和方块实体数据；没挂钩子时什么都不做 */
struct NetHooks {
    void (*block_updated)(Space &sp, BlockPos pos, mcc_state_t old, mcc_state_t now, int flags);
    void (*block_entity_data)(BlockEntity *be);
};
extern NetHooks g_net_hooks;

/* 原版 Level.sendBlockUpdated —— 把方块变化发给附近客户端 */
MCC_INLINE void net_block_updated(Space &sp, BlockPos p, mcc_state_t old, mcc_state_t now, int flags) {
    if (g_net_hooks.block_updated) g_net_hooks.block_updated(sp, p, old, now, flags);
}
/* TODO(网络)：原版 Level.levelEvent（2001 = 方块破坏粒子与音效）*/
MCC_INLINE void net_level_event(Space &, int, BlockPos, int) {}
/* 机械动力 SmartBlockEntity.sendData —— 方块实体数据发给客户端 */
MCC_INLINE void net_block_entity_data(BlockEntity *be) {
    if (g_net_hooks.block_entity_data) g_net_hooks.block_entity_data(be);
}

/* TODO(流体)：原版含水方块 updateShape 里的 scheduleTick(流体) */
MCC_INLINE void fluid_schedule_tick(Space &, BlockPos) {}

} // namespace mcc

#endif
