/*
 * 客户端世界：方块调色板、区块方块数组、区块网格、机械动力零件
 */
#ifndef MCC_CLIENT_WORLD_H
#define MCC_CLIENT_WORLD_H

#include "raylib.h"
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcc::client {

constexpr int MINY = -64, HEIGHT = 384;

/* 调色板标志（服务端消息 10） */
enum { F_AIR = 1, F_FULL = 2, F_LIQ = 4, F_COL = 8 };
/* 渲染种类 */
enum { K_CUBE = 0, K_SHAFT, K_COG, K_LCOG, K_ENC, K_GEARBOX, K_SPLIT, K_CHAIN, K_MOTOR, K_CTRL, K_ENC_COG, K_ENC_LCOG };

struct PalEntry {
    uint8_t r, g, b, flags, kind, axis, facing;
    uint8_t box[6]; /* 0~16 */
};

struct BlockName {
    uint32_t first, count;
    std::string name;
};

struct Chunk {
    int32_t cx, cz;
    std::vector<uint16_t> data;  /* (y - MINY) * 256 + lz * 16 + lx */
    std::vector<Mesh> meshes;    /* 不透明部分（顶点超过 6.5 万时分成多块） */
    std::vector<Mesh> water;
    int ymin = 9999, ymax = -9999;
    bool dirty = true;
};

/* 方向（与服务端一致）：DOWN UP NORTH SOUTH WEST EAST */
extern const int DIRV[6][3];
inline int dir_axis(int d) { return d <= 1 ? 1 : d <= 3 ? 2 : 0; }
inline bool dir_positive(int d) { return d == 1 || d == 3 || d == 5; }

struct World {
    std::vector<PalEntry> pal;
    std::vector<BlockName> names;
    std::unordered_map<int64_t, Chunk> chunks;
    std::unordered_map<int64_t, uint32_t> kinetics;   /* 坐标键 → 状态 */
    std::unordered_map<int64_t, float> speeds;        /* 坐标键 → 转速 RPM */

    static int64_t ckey(int32_t cx, int32_t cz) { return ((int64_t)cx << 32) ^ (uint32_t)cz; }
    static int64_t pkey(int32_t x, int32_t y, int32_t z) {
        return ((int64_t)(x & 0x3FFFFFF) << 38) | ((int64_t)(z & 0x3FFFFFF) << 12) | (int64_t)(y & 0xFFF);
    }

    bool ready() const { return !pal.empty(); }
    uint16_t get(int32_t x, int32_t y, int32_t z) const;
    void set(int32_t x, int32_t y, int32_t z, uint16_t s);
    const PalEntry &p(uint32_t s) const { return pal[s]; }
    bool air(uint32_t s) const { return s == 0 || (pal[s].flags & F_AIR); }
    bool full(uint32_t s) const { return s && (pal[s].flags & F_FULL); }
    bool liquid_only(uint32_t s) const { return s && (pal[s].flags & F_LIQ) && !(pal[s].flags & F_COL); }
    const std::string &name_of(uint32_t s) const;
    const BlockName *block_by_name(const std::string &n) const;

    /* 处理服务端消息；返回 true 表示认得 */
    bool handle(const uint8_t *m, size_t n);
    void clear();
    /* 每帧最多重建几个区块的网格 */
    void rebuild(int budget);
    void draw(Material &mat);
    void draw_water(Material &mat);
    void draw_kinetics(Material &mat, double t);
};

void world_init_meshes();

} // namespace mcc::client

#endif
