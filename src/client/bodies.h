/*
 * 物理结构：服务端发来形状（消息 2）和位姿（消息 3），客户端插值后
 * 用一次实例化绘制画出全部方块（每个方块一个实例：中心、旋转、颜色）。
 */
#ifndef MCC_CLIENT_BODIES_H
#define MCC_CLIENT_BODIES_H

#include "raylib.h"
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mcc::client {

struct Sample {
    double t;
    double p[3];
    float q[4]; /* x y z w */
    float com[3];
};

struct Body {
    std::vector<int16_t> blocks;        /* 每块 3 个：本地最小角 */
    std::vector<uint8_t> colors;        /* 每块 rgb */
    std::unordered_set<int64_t> lookup; /* 拾取用 */
    std::vector<Sample> samples;
    float radius = 0;                   /* 以质心为球心的包围半径 */
    /* 插值后的当前位姿 */
    bool visible = false;
    Vector3 pos{}, com{};
    Quaternion rot{ 0, 0, 0, 1 };
};

struct BodyHit {
    int id;
    float dist;
    Vector3 point;
};

struct Bodies {
    std::unordered_map<int32_t, Body> map;
    double latest_t = -1, latest_recv = 0, clock = -1, last_frame = 0;

    bool handle(const uint8_t *m, size_t n, double now_ms);
    void clear();
    void interpolate(double now_ms);
    void draw(bool depth_only);
    bool pick(Vector3 o, Vector3 d, float max_dist, BodyHit *out) const;
    size_t block_count() const;
};

void bodies_init();
extern Vector3 g_view_pos; /* 相机位置（雾、水面反光用） */

} // namespace mcc::client

#endif
