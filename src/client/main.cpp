/*
 * mcc 测试客户端（原生程序）：连本机调试服务端，第一人称走、跳、飞，
 * 放/拆机械动力零件，物理枪抓物理结构。
 *
 * 用法：mcc_client [服务端地址=127.0.0.1] [端口=8080]
 * 连不上本机服务端时，自动启动同目录下的 mcc_debug，退出时一起关掉。
 */
#include "bodies.h"
#include "font.h"
#include "net.h"
#include "process.h"
#include "render.h"
#include "sound.h"
#include "world.h"

#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

using namespace mcc::client;

/* ================================================================== */
/* 文字                                                                */
/* ================================================================== */

static const char *PRELOAD =
    "物理枪传动杆齿轮大创造马达箱离合器换向机壳石头转速控制链式可调黄铜木板玻璃泥土圆铁块"
    "连接中断开重新已启动服务端找不到正在每刻最慢区域结构在动帧秒下行飞行网络个部件应力不里来自设定调整抓着距离超载"
    "开始召唤清空继续退出移动跳跑潜行开关时上升降或滚轮选物品破坏右键放置反向看拖它远近松扔出去冻结解菜单这第页切换左";


static void text(const char *s, float x, float y, float size, Color c) {
    const Font &f = font_get(s);
    DrawTextEx(f, s, Vector2{ x, y }, size, 0, c);
}

static Vector2 measure(const char *s, float size) { return MeasureTextEx(font_get(s), s, size, 0); }

/* 界面配色：半条命那种琥珀色 */
static const Color HUD{ 255, 206, 72, 255 }, HUD_DIM{ 255, 206, 72, 140 }, HUD_BG{ 0, 0, 0, 105 };

/* 半透明底的多行文字框 */
static void panel(const std::string &s, float x, float y, float size, bool right_align = false) {
    Vector2 m = measure(s.c_str(), size);
    float w = m.x + 20, h = m.y + 12;
    if (right_align) x -= w;
    DrawRectangleRounded(Rectangle{ x, y, w, h }, 0.12f, 6, HUD_BG);
    text(s.c_str(), x + 10, y + 6, size, HUD);
}

/* ================================================================== */
/* 玩家                                                                */
/* ================================================================== */

static World g_world;
static Bodies g_bodies;

struct Player {
    Vector3 p{ 2.5f, 66, -12.5f }, v{};
    float yaw = PI, pitch = -0.15f;
    bool on_ground = false, fly = false;
};
static Player g_pl;
static constexpr float PW = 0.3f, PH = 1.8f, EYE = 1.62f;

static Vector3 eye() { return Vector3{ g_pl.p.x, g_pl.p.y + EYE, g_pl.p.z }; }
static Vector3 look_dir() {
    return Vector3Normalize(Vector3{ -sinf(g_pl.yaw) * cosf(g_pl.pitch), sinf(g_pl.pitch), -cosf(g_pl.yaw) * cosf(g_pl.pitch) });
}

static const float FULL_BOX[6] = { 0, 0, 0, 1, 1, 1 };
static void box_of(uint32_t s, float b[6]) {
    if (g_world.full(s)) { memcpy(b, FULL_BOX, sizeof FULL_BOX); return; }
    const PalEntry &e = g_world.p(s);
    for (int k = 0; k < 6; k++) b[k] = e.box[k] / 16.f;
}

/* 按轴移动并推出碰撞（原版 Entity.collide 的做法：先 y 再 x、z） */
static float move_axis(int ax, float d) {
    if (d == 0) return 0;
    float *P = &g_pl.p.x;
    float plo[3] = { P[0] - PW, P[1], P[2] - PW }, phi[3] = { P[0] + PW, P[1] + PH, P[2] + PW };
    float lo[3] = { plo[0], plo[1], plo[2] }, hi[3] = { phi[0], phi[1], phi[2] };
    lo[ax] += fminf(d, 0);
    hi[ax] += fmaxf(d, 0);
    for (int x = (int)floorf(lo[0]); x <= (int)floorf(hi[0]); x++)
        for (int y = (int)floorf(lo[1]) - 1; y <= (int)floorf(hi[1]); y++)
            for (int z = (int)floorf(lo[2]); z <= (int)floorf(hi[2]); z++) {
                uint32_t s = g_world.get(x, y, z);
                if (!s || !(g_world.p(s).flags & F_COL)) continue;
                float bb[6];
                box_of(s, bb);
                float b[6] = { x + bb[0], y + bb[1], z + bb[2], x + bb[3], y + bb[4], z + bb[5] };
                bool overlap = true;
                for (int k = 0; k < 3; k++)
                    if (k != ax && (phi[k] <= b[k] + 1e-5f || plo[k] >= b[k + 3] - 1e-5f)) overlap = false;
                if (!overlap) continue;
                if (d > 0 && phi[ax] <= b[ax] + 1e-5f) d = fminf(d, b[ax] - phi[ax]);
                else if (d < 0 && plo[ax] >= b[ax + 3] - 1e-5f) d = fmaxf(d, b[ax + 3] - plo[ax]);
            }
    P[ax] += d;
    return d;
}

static void step_player(float dt) {
    Vector3 fwd{ -sinf(g_pl.yaw), 0, -cosf(g_pl.yaw) }, right{ -fwd.z, 0, fwd.x };
    Vector3 want{};
    if (IsKeyDown(KEY_W)) want = Vector3Add(want, fwd);
    if (IsKeyDown(KEY_S)) want = Vector3Subtract(want, fwd);
    if (IsKeyDown(KEY_D)) want = Vector3Add(want, right);
    if (IsKeyDown(KEY_A)) want = Vector3Subtract(want, right);
    if (Vector3LengthSqr(want) > 0) want = Vector3Normalize(want);
    bool sneak = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    float speed = g_pl.fly ? 11 : IsKeyDown(KEY_LEFT_CONTROL) ? 5.6f : sneak ? 1.3f : 4.3f;
    Vector3 &v = g_pl.v;
    float accel = g_pl.on_ground || g_pl.fly ? 12 : 3;
    v.x += (want.x * speed - v.x) * fminf(1, accel * dt);
    v.z += (want.z * speed - v.z) * fminf(1, accel * dt);
    if (g_pl.fly) {
        float up = (IsKeyDown(KEY_SPACE) ? 1.f : 0.f) - (sneak ? 1.f : 0.f);
        v.y += (up * 8 - v.y) * fminf(1, 10 * dt);
    } else {
        v.y -= 32 * dt;
        if (v.y < -78) v.y = -78;
        if (IsKeyDown(KEY_SPACE) && g_pl.on_ground) v.y = 9;
    }
    /* 小步走，避免一步穿过方块 */
    float fastest = fmaxf(fabsf(v.x), fmaxf(fabsf(v.y), fabsf(v.z)));
    int n = (int)fmaxf(1, ceilf(fastest * dt / 0.3f));
    g_pl.on_ground = false;
    for (int i = 0; i < n; i++) {
        float h = dt / n;
        float dy = move_axis(1, v.y * h);
        if (fabsf(dy - v.y * h) > 1e-6f) {
            if (v.y < 0) g_pl.on_ground = true;
            v.y = 0;
        }
        float dx = move_axis(0, v.x * h);
        if (fabsf(dx - v.x * h) > 1e-6f) v.x = 0;
        float dz = move_axis(2, v.z * h);
        if (fabsf(dz - v.z * h) > 1e-6f) v.z = 0;
    }
    if (g_pl.p.y < -80) {
        g_pl.p = Vector3{ 2.5f, 70, -12.5f };
        g_pl.v = Vector3{};
    }
}

/* ================================================================== */
/* 选中方块（体素 DDA）                                                 */
/* ================================================================== */

struct Hit {
    int x, y, z, face;
    uint32_t s;
};

static bool raycast_block(Vector3 o, Vector3 d, float max_dist, Hit *h) {
    int x = (int)floorf(o.x), y = (int)floorf(o.y), z = (int)floorf(o.z);
    int sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1, sz = d.z > 0 ? 1 : -1;
    float tdx = fabsf(1 / d.x), tdy = fabsf(1 / d.y), tdz = fabsf(1 / d.z);
    float tx = (d.x > 0 ? (x + 1 - o.x) : (o.x - x)) * tdx;
    float ty = (d.y > 0 ? (y + 1 - o.y) : (o.y - y)) * tdy;
    float tz = (d.z > 0 ? (z + 1 - o.z) : (o.z - z)) * tdz;
    int face = -1;
    float t = 0;
    for (int i = 0; i < 256 && t <= max_dist; i++) {
        uint32_t s = g_world.get(x, y, z);
        if (s && !g_world.air(s) && !g_world.liquid_only(s)) {
            const PalEntry &e = g_world.p(s);
            bool hit = g_world.full(s) || e.kind != K_CUBE;
            if (!hit && e.box[3] > e.box[0]) {
                /* 不是整块的：再和它的外框求交 */
                float b[6];
                box_of(s, b);
                float t0 = 0, t1 = 1e9f, oo[3] = { o.x, o.y, o.z }, dd[3] = { d.x, d.y, d.z };
                int base[3] = { x, y, z };
                for (int k = 0; k < 3; k++) {
                    float a = (base[k] + b[k] - oo[k]) / dd[k], c = (base[k] + b[k + 3] - oo[k]) / dd[k];
                    t0 = fmaxf(t0, fminf(a, c));
                    t1 = fminf(t1, fmaxf(a, c));
                }
                hit = t0 <= t1;
            }
            if (hit) {
                *h = Hit{ x, y, z, face, s };
                return true;
            }
        }
        if (tx < ty && tx < tz) { x += sx; t = tx; tx += tdx; face = sx > 0 ? 4 : 5; }
        else if (ty < tz) { y += sy; t = ty; ty += tdy; face = sy > 0 ? 0 : 1; }
        else { z += sz; t = tz; tz += tdz; face = sz > 0 ? 2 : 3; }
    }
    return false;
}

/* ================================================================== */
/* 手感：走路晃动、碎块粒子、手里的东西、物品栏图标                     */
/* ================================================================== */

struct Particle {
    Vector3 p, v;
    Color c;
    float life, size;
};
static std::vector<Particle> g_parts;
static float g_walk, g_bob, g_fov = 72, g_swing;
static uint32_t g_rng = 99;
static float frand() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / (float)0xFFFFFF;
}

static void break_particles(int x, int y, int z, Color c) {
    for (int i = 0; i < 28; i++) {
        float k = 0.65f + 0.35f * frand();
        Particle p;
        p.p = Vector3{ x + 0.15f + 0.7f * frand(), y + 0.15f + 0.7f * frand(), z + 0.15f + 0.7f * frand() };
        p.v = Vector3{ (frand() - 0.5f) * 4, frand() * 4 + 1, (frand() - 0.5f) * 4 };
        p.c = Color{ (unsigned char)(c.r * k), (unsigned char)(c.g * k), (unsigned char)(c.b * k), 255 };
        p.life = 0.6f + 0.5f * frand();
        p.size = 0.07f + 0.07f * frand();
        g_parts.push_back(p);
    }
}

static void update_particles(float dt) {
    for (auto &p : g_parts) {
        p.v.y -= 22 * dt;
        Vector3 n = Vector3Add(p.p, Vector3Scale(p.v, dt));
        uint32_t s = g_world.get((int)floorf(n.x), (int)floorf(n.y - p.size / 2), (int)floorf(n.z));
        if (s && g_world.full(s)) {
            p.v = Vector3Scale(p.v, 0.3f);
            p.v.y = 0;
            n = Vector3{ p.p.x, p.p.y, p.p.z };
        }
        p.p = n;
        p.life -= dt;
    }
    std::erase_if(g_parts, [](const Particle &p) { return p.life <= 0; });
}

/* 相机朝向的变换：x 右、y 上、z 后 */
static Matrix view_basis(Vector3 pos, Vector3 f) {
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, Vector3{ 0, 1, 0 })), u = Vector3CrossProduct(r, f);
    Matrix m = MatrixIdentity();
    m.m0 = r.x; m.m1 = r.y; m.m2 = r.z;
    m.m4 = u.x; m.m5 = u.y; m.m6 = u.z;
    m.m8 = -f.x; m.m9 = -f.y; m.m10 = -f.z;
    m.m12 = pos.x; m.m13 = pos.y; m.m14 = pos.z;
    return m;
}

/* ================================================================== */
/* 物品栏、放置                                                        */
/* ================================================================== */

enum Place { P_GUN, P_AXIS, P_COG, P_GEARBOX, P_FACING, P_HAXIS, P_PLAIN };
struct Slot {
    const char *label, *block;
    Place place;
    Color color;
};
static const Slot SLOTS[2][10] = {
    {
        { "物理枪", nullptr, P_GUN, { 34, 211, 238, 255 } },
        { "传动杆", "create:shaft", P_AXIS, { 181, 181, 174, 255 } },
        { "齿轮", "create:cogwheel", P_COG, { 156, 110, 60, 255 } },
        { "大齿轮", "create:large_cogwheel", P_COG, { 122, 85, 48, 255 } },
        { "创造马达", "create:creative_motor", P_FACING, { 165, 80, 200, 255 } },
        { "齿轮箱", "create:gearbox", P_GEARBOX, { 160, 155, 140, 255 } },
        { "离合器", "create:clutch", P_AXIS, { 192, 57, 43, 255 } },
        { "换向器", "create:gearshift", P_AXIS, { 46, 134, 193, 255 } },
        { "机壳传动杆", "create:andesite_encased_shaft", P_AXIS, { 160, 155, 140, 255 } },
        { "石头", "minecraft:stone", P_PLAIN, { 128, 128, 128, 255 } },
    },
    {
        { "转速控制器", "create:rotation_speed_controller", P_HAXIS, { 205, 160, 75, 255 } },
        { "链式传动", "create:encased_chain_drive", P_AXIS, { 160, 155, 140, 255 } },
        { "可调链式", "create:adjustable_chain_gearshift", P_AXIS, { 200, 165, 90, 255 } },
        { "黄铜机壳杆", "create:brass_encased_shaft", P_AXIS, { 205, 160, 75, 255 } },
        { "机壳齿轮", "create:andesite_encased_cogwheel", P_AXIS, { 160, 155, 140, 255 } },
        { "木板", "minecraft:oak_planks", P_PLAIN, { 175, 140, 85, 255 } },
        { "玻璃", "minecraft:glass", P_PLAIN, { 190, 220, 230, 255 } },
        { "泥土", "minecraft:dirt", P_PLAIN, { 125, 90, 60, 255 } },
        { "圆石", "minecraft:cobblestone", P_PLAIN, { 115, 115, 115, 255 } },
        { "铁块", "minecraft:iron_block", P_PLAIN, { 210, 210, 210, 255 } },
    },
};
static int g_page, g_sel;

static const char AXN[3] = { 'x', 'y', 'z' };
static const char *FACING_NAME[6] = { "down", "up", "north", "south", "west", "east" };

static int nearest_look_axis(Vector3 d) {
    float a[3] = { fabsf(d.x), fabsf(d.y), fabsf(d.z) };
    return a[0] > a[1] && a[0] > a[2] ? 0 : a[1] > a[2] ? 1 : 2;
}
static int nearest_look_dir(Vector3 d) {
    int a = nearest_look_axis(d);
    bool pos = (a == 0 ? d.x : a == 1 ? d.y : d.z) > 0;
    return a == 0 ? (pos ? 5 : 4) : a == 1 ? (pos ? 1 : 0) : (pos ? 3 : 2);
}

/* 相邻的动力部件是否有轴伸向 d 方向（按渲染种类近似 IRotate.hasShaftTowards） */
static bool shaft_towards(uint32_t s, int d) {
    const PalEntry &e = g_world.p(s);
    switch (e.kind) {
    case K_SHAFT: case K_ENC: case K_SPLIT: case K_CHAIN: case K_COG: case K_LCOG: case K_CTRL: return dir_axis(d) == e.axis;
    case K_GEARBOX: return dir_axis(d) != e.axis;
    case K_MOTOR: return e.facing == d;
    }
    return false;
}

static int preferred_axis(int x, int y, int z) {
    int pref = -1;
    for (int d = 0; d < 6; d++) {
        uint32_t s = g_world.get(x + DIRV[d][0], y + DIRV[d][1], z + DIRV[d][2]);
        if (!s || g_world.air(s) || g_world.p(s).kind == K_CUBE) continue;
        if (!shaft_towards(s, d ^ 1)) continue;
        if (pref >= 0 && pref != dir_axis(d)) return -1;
        pref = dir_axis(d);
    }
    return pref;
}

static void place(const Slot &slot, const Hit &hit, bool sneak) {
    int x = hit.x + DIRV[hit.face][0], y = hit.y + DIRV[hit.face][1], z = hit.z + DIRV[hit.face][2];
    Vector3 d = look_dir();
    char props[64] = "";
    switch (slot.place) {
    case P_AXIS: {
        int pa = preferred_axis(x, y, z);
        snprintf(props, sizeof props, "axis=%c", AXN[pa >= 0 && !sneak ? pa : nearest_look_axis(d)]);
        break;
    }
    case P_COG: {
        int ax = dir_axis(hit.face);
        if (!sneak && g_world.p(hit.s).kind == K_COG) ax = g_world.p(hit.s).axis;
        snprintf(props, sizeof props, "axis=%c", AXN[ax]);
        break;
    }
    case P_GEARBOX: snprintf(props, sizeof props, "axis=y"); break;
    case P_FACING: {
        int f = nearest_look_dir(d);
        snprintf(props, sizeof props, "facing=%s", FACING_NAME[sneak ? f : f ^ 1]);
        break;
    }
    case P_HAXIS: snprintf(props, sizeof props, "axis=%c", fabsf(d.x) > fabsf(d.z) ? 'z' : 'x'); break;
    default: break;
    }
    /* 不能放进自己身体里 */
    const BlockName *b = g_world.block_by_name(slot.block);
    if (!b) return;
    Vector3 p = g_pl.p;
    if (x + 1 > p.x - PW && x < p.x + PW && y + 1 > p.y && y < p.y + PH && z + 1 > p.z - PW && z < p.z + PW &&
        (g_world.p(b->first).flags & F_COL))
        return;
    char cmd[200];
    snprintf(cmd, sizeof cmd, "place %d %d %d %s %s", x, y, z, slot.block, props);
    net_send(cmd);
}

/* 手里的东西画在镜头右下角，走路时跟着晃，放拆时挥一下 */
static void draw_held(const Slot &slot, Vector3 cam, Vector3 f, float t, bool holding) {
    float phase = g_walk * PI / 1.8f;
    float sx = sinf(phase) * 0.025f * g_bob, sy = -fabsf(cosf(phase)) * 0.03f * g_bob;
    float sw = sinf(g_swing * PI);
    Matrix vb = view_basis(cam, f);
    rlDisableDepthTest();
    if (slot.place == P_GUN) {
        /* 物理枪：枪身、握把、发光的枪口 */
        Vector3 at{ 0.32f + sx, -0.3f + sy - sw * 0.04f, -0.78f };
        Matrix base = MatrixMultiply(MatrixMultiply(MatrixScale(0.7f, 0.7f, 0.7f), MatrixRotateY(0.1f)), MatrixTranslate(at.x, at.y, at.z));
        auto part = [&](Vector3 c, Vector3 s, Color col) {
            world_draw_item(g_render.lit_mat, nullptr, col, MatrixMultiply(MatrixMultiply(MatrixScale(s.x, s.y, s.z), MatrixTranslate(c.x, c.y, c.z)),
                                                                          MatrixMultiply(base, vb)));
        };
        part({ 0, 0, 0 }, { 0.09f, 0.1f, 0.34f }, Color{ 70, 75, 85, 255 });
        part({ 0, -0.09f, 0.1f }, { 0.06f, 0.14f, 0.07f }, Color{ 45, 45, 50, 255 });
        part({ 0, 0.065f, -0.02f }, { 0.05f, 0.03f, 0.22f }, Color{ 34, 180, 210, 255 });
        part({ 0, 0, -0.2f }, { 0.06f, 0.06f, 0.08f }, Color{ 90, 95, 105, 255 });
        float glow = 0.6f + 0.4f * sinf(t * (holding ? 18.f : 3.f));
        DrawSphere(Vector3Transform(Vector3{ 0, 0, -0.25f }, MatrixMultiply(base, vb)), 0.028f,
                   Color{ 120, 235, 255, (unsigned char)(150 + 100 * glow) });
    } else {
        Vector3 at{ 0.34f + sx, -0.32f + sy - sw * 0.08f, -0.6f - sw * 0.1f };
        Matrix local = MatrixMultiply(MatrixMultiply(MatrixScale(0.26f, 0.26f, 0.26f), MatrixRotateY(0.75f + sw * 0.3f)),
                                      MatrixRotateX(0.15f - sw * 0.6f));
        world_draw_item(g_render.lit_mat, slot.block, slot.color, MatrixMultiply(MatrixMultiply(local, MatrixTranslate(at.x, at.y, at.z)), vb));
    }
    rlEnableDepthTest();
}

static Color scale_color(Color c, float k) {
    return Color{ (unsigned char)fminf(255, c.r * k), (unsigned char)fminf(255, c.g * k), (unsigned char)fminf(255, c.b * k), c.a };
}

/* 两面都画，不用管三角形绕向 */
static void tri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    DrawTriangle(a, b, c, col);
    DrawTriangle(a, c, b, col);
}

static void draw_icon(const Slot &s, float cx, float cy) {
    if (s.place == P_GUN) {
        DrawRectanglePro(Rectangle{ cx - 14, cy - 5, 28, 10 }, Vector2{ 0, 0 }, 0, Color{ 80, 85, 95, 255 });
        DrawRectangle((int)cx - 10, (int)cy + 4, 7, 10, Color{ 50, 50, 55, 255 });
        DrawRectangle((int)cx - 12, (int)cy - 8, 20, 3, s.color);
        DrawCircle((int)cx + 15, (int)cy, 4, Color{ 150, 240, 255, 255 });
        return;
    }
    std::string b = s.block;
    if (b == "create:shaft") {
        DrawRectanglePro(Rectangle{ cx, cy, 6, 26 }, Vector2{ 3, 13 }, 45, scale_color(s.color, 0.8f));
        DrawRectanglePro(Rectangle{ cx, cy, 3, 26 }, Vector2{ 2, 13 }, 45, s.color);
        return;
    }
    if (b == "create:cogwheel" || b == "create:large_cogwheel") {
        float r = b == "create:cogwheel" ? 9 : 12;
        int teeth = b == "create:cogwheel" ? 8 : 12;
        Color dark = scale_color(s.color, 0.75f);
        for (int i = 0; i < teeth; i++)
            DrawRectanglePro(Rectangle{ cx, cy, 5, 2 * r + 6 }, Vector2{ 2.5f, r + 3 }, 180.f * i / teeth, dark);
        DrawCircle((int)cx, (int)cy, r, s.color);
        DrawCircle((int)cx, (int)cy, 3, Color{ 181, 181, 174, 255 });
        return;
    }
    /* 其余：斜着看的小方块，三个面三种亮度 */
    float h = 11;
    Vector2 top{ cx, cy - h }, l{ cx - h, cy - h / 2 }, r{ cx + h, cy - h / 2 }, c{ cx, cy };
    Vector2 lb{ cx - h, cy + h / 2 }, rb{ cx + h, cy + h / 2 }, bot{ cx, cy + h };
    tri(top, l, c, s.color); tri(top, c, r, s.color);
    tri(l, lb, bot, scale_color(s.color, 0.78f)); tri(l, bot, c, scale_color(s.color, 0.78f));
    tri(c, bot, rb, scale_color(s.color, 0.6f)); tri(c, rb, r, scale_color(s.color, 0.6f));
}

/* ================================================================== */
/* 服务端回复                                                          */
/* ================================================================== */

struct Info {
    bool valid = false;
    int x, y, z;
    uint32_t state;
    bool kinetic, over, has_src;
    float speed, stress, cap;
    int size, src[3], value;
};
static Info g_info;
static std::string g_tps = "TPS -";

template <class T> static T rd(const uint8_t *p) { T v; memcpy(&v, p, sizeof v); return v; }

static void handle_misc(const uint8_t *m, size_t n) {
    if (m[0] == 11 && n >= 18) {
        Info r;
        r.valid = true;
        r.x = rd<int32_t>(m + 1); r.y = rd<int32_t>(m + 5); r.z = rd<int32_t>(m + 9);
        r.state = rd<uint32_t>(m + 13);
        r.kinetic = m[17];
        if (r.kinetic && n >= 52) {
            r.speed = rd<float>(m + 18); r.over = m[22]; r.stress = rd<float>(m + 23); r.cap = rd<float>(m + 27);
            r.size = rd<int32_t>(m + 31); r.has_src = m[35];
            for (int k = 0; k < 3; k++) r.src[k] = rd<int32_t>(m + 36 + k * 4);
            r.value = rd<int32_t>(m + 48);
        } else r.kinetic = false;
        g_info = r;
    } else if (m[0] == 5 && n >= 29) {
        char buf[200];
        snprintf(buf, sizeof buf, "TPS %.1f  每刻 %.2fms（最慢 %.2f）\n区域 %u  结构 %u（在动 %u）", rd<float>(m + 5), rd<float>(m + 9),
                 rd<float>(m + 13), rd<uint32_t>(m + 17), rd<uint32_t>(m + 25), rd<uint32_t>(m + 21));
        g_tps = buf;
    }
}

/* ================================================================== */
/* 主程序                                                              */
/* ================================================================== */

struct Held {
    bool on = false;
    int id;
    float dist;
};

static Held g_held;
static double g_last_hold;

static void spawn_at_look(int n) {
    Hit h;
    Vector3 at = raycast_block(eye(), look_dir(), 60, &h) ? Vector3{ (float)h.x, (float)h.y, (float)h.z }
                                                          : Vector3Add(eye(), Vector3Scale(look_dir(), 12));
    char cmd[96];
    snprintf(cmd, sizeof cmd, "spawn %.1f %.1f %d", at.x, at.z, n);
    net_send(cmd);
}

static void release() {
    if (g_held.on) {
        net_send("release");
        sound_play(SFX_DROP, 0.6f);
    }
    g_held.on = false;
}

/* 菜单项：左边一列文字，鼠标移上去变亮、前面出一个小三角 */
static bool menu_item(const char *label, float x, float y, bool enabled = true) {
    Vector2 m = measure(label, 26);
    Rectangle r{ x - 24, y - 4, m.x + 48, m.y + 8 };
    bool hover = enabled && CheckCollisionPointRec(GetMousePosition(), r);
    if (hover) DrawTriangle(Vector2{ x - 18, y + 7 }, Vector2{ x - 18, y + 23 }, Vector2{ x - 8, y + 15 }, HUD);
    text(label, x, y, 26, !enabled ? Color{ 120, 120, 120, 255 } : hover ? HUD : Color{ 215, 215, 210, 255 });
    bool click = hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (click) sound_play(SFX_CLICK, 0.5f, 0.02f);
    return click;
}

static const char *HELP[] = {
    "WASD 移动  空格 跳  Ctrl 跑  Shift 潜行",
    "F 或双击空格：飞行开关（飞行时空格上升、Shift 下降）",
    "1~0 或滚轮选物品，Tab 切换第二页",
    "左键破坏，右键放置（Shift+右键：马达反向放）",
    "看着马达或转速控制器按 [ ] 调转速（±8 RPM）",
    "物理枪（第 1 格）：按住左键抓结构，转视角拖它，滚轮调远近，",
    "    松开扔出去；抓着时右键冻结，对冻结的结构右键解冻",
    "R 在看着的地方召唤一个物理结构   Esc 打开这个菜单",
};

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    bool local = !strcmp(host, "127.0.0.1") || !strcmp(host, "localhost");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 760, "mcc 测试客户端");
    SetExitKey(KEY_NULL);
    font_init(22, PRELOAD);
    render_init();
    world_init_meshes();
    bodies_init();
    sound_init();

    bool menu = true, started_server = false, ever_connected = false, was_up = false, attempting = false;
    double next_try = 0, last_space = -1, info_at = 0, stat_at = GetTime();
    int frames = 0, fps = 0;
    size_t bytes_mark = 0;
    float kbps = 0;
    std::string status = "连接中…";
    int64_t info_key = INT64_MIN;
    std::vector<Message> inbox;
    /* 测试用：MCC_CLIENT_SHOT=N 连上后召唤 N 个结构，过几秒截图退出（=menu 时截菜单） */
    const char *shot_env = getenv("MCC_CLIENT_SHOT");
    bool screenshot = shot_env != nullptr, shot_spawned = false;
    double first_data = -1;

    bool quit = false;
    while (!WindowShouldClose() && !quit) {
        double now = GetTime();
        float dt = fminf(0.05f, GetFrameTime());

        /* ---------- 连接（后台连，不卡画面） ---------- */
        if (was_up && !net_connected()) {
            was_up = false;
            g_world.clear();
            g_bodies.clear();
            g_held.on = false;
            status = "连接断开，重新连接中…";
            next_try = now + 2;
        }
        if (net_connecting()) {
        } else if (attempting) {
            attempting = false;
            if (net_connected()) {
                ever_connected = was_up = true;
                status = "已连接";
            } else if (local && !started_server && !ever_connected) {
                started_server = start_server(GetApplicationDirectory(), port);
                status = started_server ? "正在启动服务端…" : "找不到服务端（mcc_debug）";
                next_try = now + 0.5;
            } else next_try = now + (started_server && !ever_connected ? 0.5 : 2.0);
        } else if (!net_connected() && now >= next_try) {
            net_connect_async(host, port);
            attempting = true;
        }

        inbox.clear();
        net_poll(inbox);
        double now_ms = now * 1000;
        for (auto &m : inbox) {
            if (m.empty()) continue;
            if (!g_world.handle(m.data(), m.size()) && !g_bodies.handle(m.data(), m.size(), now_ms)) handle_misc(m.data(), m.size());
        }
        if (first_data < 0 && !g_world.chunks.empty()) first_data = now;
        g_world.rebuild(6);

        /* ---------- 输入 ---------- */
        bool sneak = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (menu) {
            if (IsKeyPressed(KEY_ESCAPE) && ever_connected) menu = false, DisableCursor();
        } else {
            if (IsKeyPressed(KEY_ESCAPE) || !IsWindowFocused()) {
                menu = true;
                release();
                EnableCursor();
            }
        }
        if (!menu) {
            Vector2 md = GetMouseDelta();
            g_pl.yaw -= md.x * 0.0025f;
            g_pl.pitch = Clamp(g_pl.pitch - md.y * 0.0025f, -1.55f, 1.55f);
            if (IsKeyPressed(KEY_SPACE)) {
                if (now - last_space < 0.28) g_pl.fly = !g_pl.fly; /* 双击空格切换飞行 */
                last_space = now;
            }
            if (IsKeyPressed(KEY_F)) g_pl.fly = !g_pl.fly;
            if (IsKeyPressed(KEY_TAB)) g_page ^= 1;
            int prev_sel = g_sel, prev_page = g_page;
            for (int k = 0; k < 10; k++)
                if (IsKeyPressed(KEY_ZERO + k)) g_sel = (k + 9) % 10;
            if (IsKeyPressed(KEY_R)) spawn_at_look(1);
            Hit hit;
            bool has_hit = g_world.ready() && raycast_block(eye(), look_dir(), 8, &hit);
            if ((IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_RIGHT_BRACKET)) && has_hit) {
                char cmd[96];
                snprintf(cmd, sizeof cmd, "adjust %d %d %d %d", hit.x, hit.y, hit.z, IsKeyPressed(KEY_LEFT_BRACKET) ? -8 : 8);
                net_send(cmd);
            }
            float wheel = GetMouseWheelMove();
            if (wheel != 0) {
                if (g_held.on) g_held.dist = Clamp(g_held.dist * expf(wheel * 0.15f), 2, 40);
                else g_sel = (g_sel + (wheel < 0 ? 1 : 9)) % 10;
            }
            if (g_sel != prev_sel || g_page != prev_page) sound_play(SFX_CLICK, 0.3f, 0.02f);
            const Slot &slot = SLOTS[g_page][g_sel];
            if (slot.place == P_GUN) {
                BodyHit bh;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_bodies.pick(eye(), look_dir(), 40, &bh)) {
                    g_held = Held{ true, bh.id, bh.dist };
                    sound_play(SFX_GRAB, 0.6f);
                    char cmd[128];
                    snprintf(cmd, sizeof cmd, "grab %d %.3f %.3f %.3f", bh.id, bh.point.x, bh.point.y, bh.point.z);
                    net_send(cmd);
                }
                if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                    char cmd[128];
                    if (g_held.on) {
                        Vector3 p = Vector3Add(eye(), Vector3Scale(look_dir(), g_held.dist));
                        snprintf(cmd, sizeof cmd, "freeze %d %.2f %.2f %.2f", g_held.id, p.x, p.y, p.z);
                        net_send(cmd);
                        g_held.on = false;
                        net_send("release");
                        sound_play(SFX_FREEZE, 0.5f);
                    } else if (g_bodies.pick(eye(), look_dir(), 40, &bh)) {
                        snprintf(cmd, sizeof cmd, "freeze %d %.2f %.2f %.2f", bh.id, bh.point.x, bh.point.y, bh.point.z);
                        net_send(cmd);
                        sound_play(SFX_FREEZE, 0.5f);
                    }
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) release();
            } else {
                release();
                if (has_hit && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    char cmd[96];
                    snprintf(cmd, sizeof cmd, "break %d %d %d", hit.x, hit.y, hit.z);
                    net_send(cmd);
                    const PalEntry &e = g_world.p(hit.s);
                    break_particles(hit.x, hit.y, hit.z, Color{ e.r, e.g, e.b, 255 });
                    sound_play(SFX_BREAK, 0.7f);
                    g_swing = 1;
                } else if (has_hit && hit.face >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                    place(slot, hit, sneak);
                    sound_play(SFX_PLACE, 0.8f);
                    g_swing = 1;
                }
            }
            step_player(dt);
        }
        if (g_held.on && !g_bodies.map.count(g_held.id)) g_held.on = false;
        if (g_held.on && now - g_last_hold > 0.033) {
            Vector3 t = Vector3Add(eye(), Vector3Scale(look_dir(), g_held.dist));
            char cmd[96];
            snprintf(cmd, sizeof cmd, "hold %.3f %.3f %.3f", t.x, t.y, t.z);
            net_send(cmd);
            g_last_hold = now;
        }

        /* 看着的方块：定时向服务端要详细信息 */
        Hit hit;
        bool has_hit = g_world.ready() && raycast_block(eye(), look_dir(), 8, &hit);
        if (has_hit) {
            int64_t key = World::pkey(hit.x, hit.y, hit.z);
            if (key != info_key || now - info_at > 0.4) {
                char cmd[64];
                snprintf(cmd, sizeof cmd, "info %d %d %d", hit.x, hit.y, hit.z);
                net_send(cmd);
                info_at = now;
                info_key = key;
            }
        } else info_key = INT64_MIN;

        g_bodies.interpolate(now_ms);

        /* ---------- 画 ---------- */
        update_particles(dt);
        sound_update(dt, g_held.on);
        /* 走路晃动、跑步时视野变宽 */
        float hspeed = sqrtf(g_pl.v.x * g_pl.v.x + g_pl.v.z * g_pl.v.z);
        bool walking = g_pl.on_ground && !g_pl.fly && !menu;
        if (walking) {
            float before = g_walk;
            g_walk += hspeed * dt;
            if (floorf(before / 1.8f) != floorf(g_walk / 1.8f)) sound_play(SFX_STEP, 0.35f, 0.15f);
        }
        g_bob += ((walking ? fminf(hspeed / 4.3f, 1.3f) : 0.f) - g_bob) * fminf(1, dt * 8);
        bool sprint = IsKeyDown(KEY_LEFT_CONTROL) && hspeed > 3;
        g_fov += ((sprint ? 80.f : g_pl.fly && hspeed > 8 ? 78.f : 72.f) - g_fov) * fminf(1, dt * 6);
        g_swing = fmaxf(0, g_swing - dt * 4);
        float phase = g_walk * PI / 1.8f;
        Vector3 look = look_dir();
        Vector3 right = Vector3Normalize(Vector3CrossProduct(look, Vector3{ 0, 1, 0 }));
        Vector3 cpos = Vector3Add(eye(), Vector3Add(Vector3Scale(right, sinf(phase) * 0.04f * g_bob),
                                                    Vector3{ 0, (0.035f - fabsf(cosf(phase)) * 0.07f) * g_bob, 0 }));
        Camera3D cam = {};
        cam.position = cpos;
        cam.target = Vector3Add(cpos, look);
        cam.up = Vector3{ 0, 1, 0 };
        cam.fovy = g_fov;
        cam.projection = CAMERA_PERSPECTIVE;
        g_view_pos = cpos;
        render_frame(cpos, (float)now);

        BeginDrawing();
        /* 阴影：从太阳方向把会投影的东西画进深度图 */
        render_shadow_begin(g_pl.p);
        g_world.draw(g_render.depth_mat);
        rlDisableBackfaceCulling();
        g_world.draw_kinetics(g_render.depth_mat, now);
        g_bodies.draw(true);
        render_shadow_end();
        rlEnableBackfaceCulling();

        ClearBackground(render_fog_color());
        BeginMode3D(cam);
        g_world.draw(g_render.lit_mat);
        rlDisableBackfaceCulling();
        g_world.draw_kinetics(g_render.lit_mat, now);
        rlEnableBackfaceCulling();
        g_bodies.draw(false);
        for (auto &p : g_parts) DrawCube(p.p, p.size, p.size, p.size, p.c);
        if (has_hit) {
            float b[6];
            if (g_world.full(hit.s) || g_world.p(hit.s).kind != K_CUBE || g_world.p(hit.s).box[3] <= g_world.p(hit.s).box[0])
                memcpy(b, FULL_BOX, sizeof b);
            else box_of(hit.s, b);
            DrawCubeWiresV(Vector3{ hit.x + (b[0] + b[3]) / 2, hit.y + (b[1] + b[4]) / 2, hit.z + (b[2] + b[5]) / 2 },
                           Vector3{ b[3] - b[0] + 0.006f, b[4] - b[1] + 0.006f, b[5] - b[2] + 0.006f }, Color{ 20, 20, 20, 200 });
        }
        if (g_held.on) {
            /* 物理枪的光束：从枪口到抓取点，一闪一闪 */
            Vector3 t = Vector3Add(eye(), Vector3Scale(look_dir(), g_held.dist));
            Matrix vb = view_basis(cpos, look);
            Vector3 from = Vector3Transform(Vector3{ 0.3f, -0.3f, -0.96f }, vb);
            float pulse = 0.5f + 0.5f * sinf((float)now * 18);
            DrawCylinderEx(from, t, 0.035f, 0.02f, 8, Color{ 34, 211, 238, (unsigned char)(50 + 40 * pulse) });
            DrawCylinderEx(from, t, 0.012f, 0.008f, 6, Color{ 200, 250, 255, 255 });
            DrawSphere(t, 0.08f + 0.03f * pulse, Color{ 120, 235, 255, 200 });
        }
        rlDisableBackfaceCulling();
        g_world.draw_water(g_render.lit_mat);
        rlEnableBackfaceCulling();
        render_lamp_glow(cam);
        if (!menu) draw_held(SLOTS[g_page][g_sel], cpos, look, (float)now, g_held.on);
        EndMode3D();

        int W = GetScreenWidth(), H = GetScreenHeight();
        /* 四周压暗一点 */
        DrawRectangleGradientV(0, 0, W, H / 3, Color{ 0, 0, 0, 90 }, Color{ 0, 0, 0, 0 });
        DrawRectangleGradientV(0, H - H / 3, W, H / 3, Color{ 0, 0, 0, 0 }, Color{ 0, 0, 0, 100 });
        DrawRectangleGradientH(0, 0, W / 4, H, Color{ 0, 0, 0, 80 }, Color{ 0, 0, 0, 0 });
        DrawRectangleGradientH(W - W / 4, 0, W / 4, H, Color{ 0, 0, 0, 0 }, Color{ 0, 0, 0, 80 });
        /* 准星 */
        Vector2 cc{ W / 2.f, H / 2.f };
        DrawCircleV(cc, 1.6f, HUD);
        DrawRing(cc, 10, 11.5f, 120, 240, 16, HUD_DIM);
        DrawRing(cc, 10, 11.5f, -60, 60, 16, HUD_DIM);

        /* 物品栏 */
        const float sw = 78, sh = 64, gap = 4;
        float x0 = W / 2.f - (10 * sw + 9 * gap) / 2, y0 = H - sh - 14;
        for (int i = 0; i < 10; i++) {
            const Slot &s = SLOTS[g_page][i];
            bool cur = i == g_sel;
            Rectangle r{ x0 + i * (sw + gap), y0 - (cur ? 4.f : 0.f), sw, sh };
            DrawRectangleRounded(r, 0.15f, 6, cur ? Color{ 20, 18, 12, 190 } : HUD_BG);
            DrawRectangleRoundedLinesEx(r, 0.15f, 6, cur ? 2 : 1, cur ? HUD : Color{ 255, 206, 72, 40 });
            char num[4];
            snprintf(num, sizeof num, "%d", (i + 1) % 10);
            text(num, r.x + 5, r.y + 3, 13, HUD_DIM);
            draw_icon(s, r.x + sw / 2, r.y + 23);
            Vector2 m = measure(s.label, 15);
            text(s.label, r.x + (sw - m.x) / 2, r.y + 43, 15, cur ? HUD : Color{ 200, 200, 195, 255 });
        }
        if (g_page) text("第 2 页（Tab）", x0, y0 - 30, 15, HUD_DIM);

        /* 左上：手里拿的、看着的方块 */
        {
            const Slot &slot = SLOTS[g_page][g_sel];
            std::string s = slot.label;
            if (g_pl.fly) s += "   [飞行]";
            char buf[256];
            const Info &r = g_info;
            if (has_hit && r.valid && r.x == hit.x && r.y == hit.y && r.z == hit.z) {
                const std::string &name = g_world.name_of(r.state);
                snprintf(buf, sizeof buf, "\n%s  (%d, %d, %d)", name.c_str(), r.x, r.y, r.z);
                s += buf;
                if (r.kinetic) {
                    snprintf(buf, sizeof buf, "\n转速 %.1f RPM%s", r.speed, r.over ? "  【超载】" : "");
                    s += buf;
                    if (r.size) snprintf(buf, sizeof buf, "\n网络 %d 个部件  应力 %.0f / %.0f SU", r.size, r.stress, r.cap);
                    else snprintf(buf, sizeof buf, "\n不在动力网络里");
                    s += buf;
                    if (r.has_src) {
                        snprintf(buf, sizeof buf, "\n动力来自 (%d, %d, %d)", r.src[0], r.src[1], r.src[2]);
                        s += buf;
                    }
                    if (name.find("motor") != std::string::npos || name.find("speed_controller") != std::string::npos) {
                        snprintf(buf, sizeof buf, "\n设定 %d RPM（[ ] 调整）", r.value);
                        s += buf;
                    }
                }
            } else if (has_hit) {
                snprintf(buf, sizeof buf, "\n%s  (%d, %d, %d)", g_world.name_of(hit.s).c_str(), hit.x, hit.y, hit.z);
                s += buf;
            }
            if (g_held.on) {
                snprintf(buf, sizeof buf, "\n抓着结构 #%d  距离 %.1f", g_held.id, g_held.dist);
                s += buf;
            }
            panel(s, 10, 10, 18);
        }

        /* 右上：服务端、帧率 */
        frames++;
        if (now - stat_at >= 1) {
            fps = (int)(frames / (now - stat_at));
            size_t b = net_bytes_received();
            kbps = (float)((b - bytes_mark) / (now - stat_at) / 1024);
            bytes_mark = b;
            frames = 0;
            stat_at = now;
        }
        {
            char buf[160];
            snprintf(buf, sizeof buf, "\n%d 帧/秒  下行 %.1f KB/秒  结构方块 %zu", fps, kbps, g_bodies.block_count());
            std::string s = (net_connected() ? g_tps : status) + buf;
            panel(s, (float)W - 10, 10, 16, true);
        }

        /* 菜单：压暗画面，左边标题和一列菜单项，右下角操作说明 */
        if (menu) {
            DrawRectangle(0, 0, W, H, Color{ 0, 0, 0, 170 });
            DrawRectangleGradientH(0, 0, W / 2, H, Color{ 0, 0, 0, 140 }, Color{ 0, 0, 0, 0 });
            float mx = W * 0.08f, my = H * 0.3f;
            text("MCC", mx, my - 96, 56, Color{ 235, 235, 230, 255 });
            text("测试客户端", mx + 4, my - 30, 20, HUD_DIM);
            float y = my + 30;
            if (!net_connected()) {
                text(status.c_str(), mx, y, 20, HUD);
                y += 50;
            }
            if (menu_item(net_connected() ? "开始" : "等待连接…", mx, y, net_connected())) {
                menu = false;
                DisableCursor();
            }
            if (menu_item("召唤 1 个结构", mx, y += 44)) spawn_at_look(1);
            if (menu_item("召唤 20 个", mx, y += 44)) spawn_at_look(20);
            if (menu_item("召唤 100 个", mx, y += 44)) spawn_at_look(100);
            if (menu_item("清空结构", mx, y += 44)) net_send("clear");
            if (menu_item("退出", mx, y += 60)) quit = true;
            float hy = H - 30 - (sizeof HELP / sizeof *HELP) * 24.f;
            for (size_t i = 0; i < sizeof HELP / sizeof *HELP; i++) {
                Vector2 m = measure(HELP[i], 16);
                text(HELP[i], W - 40 - m.x, hy + i * 24.f, 16, Color{ 170, 170, 165, 255 });
            }
        }
        EndDrawing();

        if (screenshot && first_data >= 0 && strcmp(shot_env, "menu") != 0) menu = false;
        if (screenshot && first_data >= 0 && !shot_spawned && now - first_data > 1) {
            char cmd[64];
            snprintf(cmd, sizeof cmd, "spawn 0 25 %d", atoi(shot_env));
            if (atoi(shot_env) > 0) net_send(cmd);
            shot_spawned = true;
        }
        /* 测试物理枪：看向一个结构抓住它 */
        if (screenshot && first_data >= 0 && !g_held.on && now - first_data > 5 && now - first_data < 5.5) {
            for (auto &kv : g_bodies.map) {
                if (!kv.second.visible) continue;
                Vector3 d = Vector3Normalize(Vector3Subtract(kv.second.pos, eye()));
                g_pl.yaw = atan2f(-d.x, -d.z);
                g_pl.pitch = asinf(d.y);
                BodyHit bh;
                if (g_bodies.pick(eye(), look_dir(), 60, &bh)) {
                    printf("拾取 #%d 距离 %.1f\n", bh.id, bh.dist);
                    g_held = Held{ true, bh.id, fminf(bh.dist, 12) };
                    char cmd[128];
                    snprintf(cmd, sizeof cmd, "grab %d %.3f %.3f %.3f", bh.id, bh.point.x, bh.point.y, bh.point.z);
                    net_send(cmd);
                    break;
                }
            }
        }
        if (screenshot && first_data >= 0 && now - first_data > 8) {
            printf("帧率 %d  结构方块 %zu\n", fps, g_bodies.block_count());
            TakeScreenshot("mcc_client_shot.png");
            break;
        }
    }

    net_shutdown();
    stop_server();
    sound_close();
    CloseWindow();
    return 0;
}
