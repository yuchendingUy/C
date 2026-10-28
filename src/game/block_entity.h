/*
 * 方块实体（对应原版 BlockEntity）
 *
 * 和原版最大的不同：原版每个方块实体每刻都推进一次，这里只推进"有事要做"的：
 *   - wake()：从本刻（或下一刻）起每刻推进，直到 tick() 返回 false
 *   - wake_at(t)：到空间时间 t 那一刻推进一次（代替原版里每刻倒数的计时器）
 * 被推进的方块实体按创建先后（seq）排序，和原版"按加入顺序推进"的结果一致；
 * 本刻推进过程中被唤醒、且排在当前位置之后的，本刻就推进（原版里它反正每刻都会推进）。
 *
 * 方块实体存在所在区块的游戏数据里，随区块在区域间搬家；被移除后延到本刻结束才释放，
 * 推进过程中别处还拿着的指针仍然有效（与原版"已移除的对象仍可读"一致）。
 */
#ifndef MCC_GAME_BLOCK_ENTITY_H
#define MCC_GAME_BLOCK_ENTITY_H

#include "types.h"
#include "../world/chunk.h"

namespace mcc {

struct Space;

constexpr uint64_t NEVER = UINT64_MAX;

struct BlockEntity {
    BlockPos pos{};
    mcc_state_t state = 0;
    uint64_t seq = 0;           /* 创建顺序（全维度递增），决定推进顺序 */
    mcc_chunk *chunk = nullptr; /* 所在区块 */
    Space *home = nullptr;      /* 所属空间（区块搬家时更新） */
    bool removed = false;
    bool active = false;        /* 已排进推进队列（本刻或下一刻） */
    bool born_in_phase = false; /* 在方块实体推进阶段里创建：原版排到下一刻才推进 */
    uint64_t born = 0;          /* 创建时的空间时间 */
    uint64_t wake_time = NEVER; /* wake_at 的时间（空间时间） */
    uint64_t ticked_at = NEVER; /* 本刻已推进（避免同一刻推进两次） */

    BlockEntity(BlockPos p, mcc_state_t s) : pos(p), state(s) {}
    virtual ~BlockEntity() = default;

    /* 推进一刻；返回 true 表示下一刻还要推进 */
    virtual bool tick() { return false; }
    /* 原版 setRemoved：方块被移除（不含区块卸载） */
    virtual void on_removed() {}
    /* 原版 onChunkUnloaded */
    virtual void on_chunk_unloaded() {}
    /* 原版 setBlockState：同种方块状态变化 */
    virtual void set_block_state(mcc_state_t s) { state = s; }
    /* 区块搬到另一个空间之后（区域合并/拆分），from 是原来的空间 */
    virtual void on_space_changed(Space &from) { (void)from; }
    /* 代替 instanceof（见 BlockBehaviour::interface） */
    virtual void *interface(int id) { (void)id; return nullptr; }

    Space &space() const { return *home; }
    uint64_t now() const;
    void wake();
    void wake_at(uint64_t time);
};

} // namespace mcc

#endif
