/*
 * 游戏层基础类型：方块坐标、方向、轴（语义与原版 BlockPos / Direction / Direction.Axis 一致）
 */
#ifndef MCC_GAME_TYPES_H
#define MCC_GAME_TYPES_H

#include "../core/common.h"

namespace mcc {

enum Axis : uint8_t { AXIS_X, AXIS_Y, AXIS_Z };

/* 顺序与原版 Direction.values() 一致：DOWN UP NORTH SOUTH WEST EAST */
enum Dir : uint8_t { DOWN, UP, NORTH, SOUTH, WEST, EAST };

constexpr Dir ALL_DIRS[6] = { DOWN, UP, NORTH, SOUTH, WEST, EAST };
/* 原版 NeighborUpdater.UPDATE_ORDER：邻居通知顺序 */
constexpr Dir UPDATE_ORDER[6] = { WEST, EAST, DOWN, UP, NORTH, SOUTH };
/* 原版 BlockBehaviour.UPDATE_SHAPE_ORDER：形状更新顺序 */
constexpr Dir UPDATE_SHAPE_ORDER[6] = { WEST, EAST, NORTH, SOUTH, DOWN, UP };

constexpr int DIR_DX[6] = { 0, 0, 0, 0, -1, 1 };
constexpr int DIR_DY[6] = { -1, 1, 0, 0, 0, 0 };
constexpr int DIR_DZ[6] = { 0, 0, -1, 1, 0, 0 };

MCC_INLINE Axis dir_axis(Dir d) { return d <= UP ? AXIS_Y : d <= SOUTH ? AXIS_Z : AXIS_X; }
MCC_INLINE bool dir_positive(Dir d) { return d == UP || d == SOUTH || d == EAST; }
MCC_INLINE Dir dir_opposite(Dir d) { return (Dir)(d ^ 1); }
/* 原版 Direction.fromAxisAndDirection */
MCC_INLINE Dir dir_of(Axis a, bool positive) {
    return a == AXIS_X ? (positive ? EAST : WEST) : a == AXIS_Y ? (positive ? UP : DOWN) : (positive ? SOUTH : NORTH);
}

struct BlockPos {
    int32_t x, y, z;
    BlockPos offset(Dir d, int n = 1) const { return { x + DIR_DX[d] * n, y + DIR_DY[d] * n, z + DIR_DZ[d] * n }; }
    BlockPos offset(int dx, int dy, int dz) const { return { x + dx, y + dy, z + dz }; }
    BlockPos sub(const BlockPos &o) const { return { x - o.x, y - o.y, z - o.z }; }
    bool operator==(const BlockPos &o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const BlockPos &o) const { return !(*this == o); }
    int32_t chunk_x() const { return x >> 4; }
    int32_t chunk_z() const { return z >> 4; }
    /* 原版 Axis.choose(x, y, z) */
    int32_t along(Axis a) const { return a == AXIS_X ? x : a == AXIS_Y ? y : z; }
    int32_t manhattan() const { return std::abs(x) + std::abs(y) + std::abs(z); }
    int64_t dist_sqr() const { return (int64_t)x * x + (int64_t)y * y + (int64_t)z * z; }
    /* 原版 BlockPos.asLong：机械动力用它做动力网络编号 */
    int64_t as_long() const {
        return ((int64_t)(x & 0x3FFFFFF) << 38) | ((int64_t)(z & 0x3FFFFFF) << 12) | (int64_t)(y & 0xFFF);
    }
};

/* 原版 Direction.getNearest(x, y, z)：按 values() 顺序取点积严格最大的方向；
 * 初值是 Float.MIN_VALUE（最小正数），所以全部点积 ≤ 0 时返回 NORTH */
MCC_INLINE Dir dir_nearest(float x, float y, float z) {
    Dir best = NORTH;
    float bd = 1.4e-45f;
    for (Dir d : ALL_DIRS) {
        float v = x * DIR_DX[d] + y * DIR_DY[d] + z * DIR_DZ[d];
        if (v > bd) { bd = v; best = d; }
    }
    return best;
}

} // namespace mcc

#endif
