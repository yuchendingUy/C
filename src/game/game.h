/*
 * 游戏层与世界层的接口：每个区域每刻推进它的地形空间和所有物理结构空间。
 */
#ifndef MCC_GAME_GAME_H
#define MCC_GAME_GAME_H

#include "space.h"
#include "../world/level.h"

namespace mcc {

/* 区域地形空间 / 物理结构空间（第一次访问时创建） */
MCC_INLINE Space &region_space(mcc_region_world *w) { return space_of(w->chunks, 0); }
MCC_INLINE Space &sublevel_space(mcc_sublevel *sl) { return space_of(&sl->blocks, sl->id); }

/* 区域线程上调用：推进本区域的全部方块空间（计划刻、方块实体） */
void game_tick_region(mcc_region_world *w);

} // namespace mcc

#endif
