/*
 * 方块空间：一个区块存储（区域的地形，或一个物理结构自己的方块）加上它的游戏逻辑状态。
 *
 * 机械动力等游戏逻辑都通过方块空间读写方块，所以同一套代码在地面上和物理结构上都能跑。
 * 一个空间只由拥有它的区域线程访问：
 *   - 区域地形空间：随区块存储在区域合并时一起交换；区块搬家时，区块里的方块实体、
 *     计划刻跟着搬到新空间（时间按两边各自的刻数换算）
 *   - 物理结构空间：跟着物理结构在区域间迁移，自带时间，不用换算
 *
 * 每刻推进顺序（照原版 ServerLevel.tick）：计划刻 → 方块实体。
 */
#ifndef MCC_GAME_SPACE_H
#define MCC_GAME_SPACE_H

#include "block.h"
#include "block_entity.h"
#include <vector>

namespace mcc {

/* 原版 ScheduledTick：同一位置同一方块只能有一个 */
struct ScheduledTick {
    uint64_t due;      /* 空间时间（区块搬家期间暂存为相对值） */
    uint64_t sub;      /* 同一刻内的先后（原版 subTickOrder） */
    BlockPos pos;
    mcc_block_t block;
    int8_t priority;
};

/* 区块的游戏数据（mcc_chunk::ext） */
struct ChunkData {
    mcc_hmap bes;                       /* 区块内坐标 → BlockEntity* */
    std::vector<ScheduledTick> ticks;
    bool relative = false;              /* 搬家途中：时间暂存为相对值 */
};

/* 原版 CollectingNeighborUpdater 的一条待办 */
struct NeighborUpdate {
    enum Kind : uint8_t { SHAPE, SIMPLE, MULTI } kind;
    Dir dir;            /* SHAPE：变化来自的方向；MULTI：跳过的方向 */
    uint8_t idx;        /* MULTI：下一个要通知的方向下标 */
    bool has_skip;
    mcc_state_t state;  /* SHAPE：来源方块的新状态 */
    BlockPos pos;       /* SHAPE/SIMPLE：被通知的方块；MULTI：来源 */
    BlockPos from;      /* SHAPE/SIMPLE：来源 */
    mcc_block_t block;  /* SIMPLE/MULTI：来源方块 */
    int flags, recursion;
};

/* 空间附加数据（各模组自己的每空间状态，例如机械动力的应力网络表） */
constexpr int MAX_SPACE_ATTACHMENTS = 4;
struct SpaceAttachmentType {
    void (*destroy)(void *data);
};
int space_attachment_register(const SpaceAttachmentType *type);

struct WakeEntry {
    uint64_t due, seq;
    BlockPos pos;
    bool operator>(const WakeEntry &o) const { return due != o.due ? due > o.due : seq > o.seq; }
};

struct Space {
    mcc_chunk_store *store;
    int32_t sublevel;               /* 0 = 区域地形，否则物理结构编号 */
    uint64_t time = 0;              /* 本空间推进过的刻数 */

    /* 方块实体推进 */
    std::vector<BlockEntity *> active;       /* 下一次推进时要推进的 */
    std::vector<BlockEntity *> current;      /* 正在推进的这一批（按 seq 的小顶堆） */
    std::vector<WakeEntry> wakes;            /* wake_at 的小顶堆 */
    bool ticking_bes = false;
    uint64_t cur_seq = 0;                    /* 正在推进的方块实体的 seq */
    std::vector<BlockEntity *> graveyard;    /* 已移除、本刻结束后释放 */

    /* 计划刻 */
    mcc_hmap tick_chunks;                    /* 有计划刻的区块 */
    uint64_t next_sub = 0;

    /* 原版 CollectingNeighborUpdater */
    std::vector<NeighborUpdate> nu_stack, nu_added;
    int nu_count = 0;

    void *attach[MAX_SPACE_ATTACHMENTS] = {};
    mcc_store_listener listener{};

    /* ---- 读 ---- */
    bool is_loaded(BlockPos p) const { return mcc_store_chunk(store, p.x >> 4, p.z >> 4) != nullptr; }
    mcc_state_t get(BlockPos p) const { return mcc_store_get(store, p.x, p.y, p.z); }
    BlockEntity *block_entity(BlockPos p) const;

    /* ---- 写（照原版 Level.setBlock，返回是否改变） ---- */
    bool set_block(BlockPos p, mcc_state_t state, int flags = UPDATE_ALL, int recursion = 512);
    /* 原版 Level.destroyBlock：变成空气（含水方块变成水），可选掉落 */
    bool destroy_block(BlockPos p, bool drop, int recursion = 512);
    /* 原版 Level.removeBlockEntity */
    void remove_block_entity(BlockPos p);

    /* ---- 方块更新 ---- */
    /* 原版 Level.updateNeighborsAt：六个邻居 neighborChanged */
    void update_neighbors_at(BlockPos p, mcc_block_t block);
    void update_neighbors_at_except(BlockPos p, mcc_block_t block, Dir skip);
    /* 原版 Level.neighborChanged：通知单个方块 */
    void neighbor_changed(BlockPos p, mcc_block_t block, BlockPos from);
    /* 原版 BlockState.updateNeighbourShapes */
    void update_neighbour_shapes(BlockPos p, mcc_state_t state, int flags, int recursion);

    /* ---- 计划刻（原版 Level.scheduleTick） ---- */
    void schedule_tick(BlockPos p, mcc_block_t block, int delay, TickPriority prio = PRIORITY_NORMAL);
    bool has_scheduled_tick(BlockPos p, mcc_block_t block) const;

    /* ---- 推进 ---- */
    void tick();

    /* 内部 */
    void be_wake(BlockEntity *be);
    void be_wake_at(BlockEntity *be, uint64_t t);
    void add_block_entity(BlockEntity *be, mcc_chunk *c);
    void chunk_added(mcc_chunk *c);
    void chunk_removed(mcc_chunk *c);
    void nu_add_and_run(const NeighborUpdate &u);
    void run_scheduled_ticks();
    void run_block_entities();
};

/* 存储对应的空间：第一次访问时创建并接管存储里已有的区块。
 * sublevel 为 0 表示区域地形 */
Space &space_of(mcc_chunk_store *store, int32_t sublevel);

/* 游戏层初始化：设置区块/存储的释放钩子（进程里调用一次） */
void game_init();

} // namespace mcc

#endif
