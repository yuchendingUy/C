#include "bodies.h"
#include "render.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <string.h>
#include <string>

namespace mcc::client {

/* 每个实例：中心 xyz、旋转四元数 xyzw、颜色 rgba8，共 32 字节 */
struct Instance {
    float c[3];
    float q[4];
    uint8_t col[4];
};
static_assert(sizeof(Instance) == 32);

static const char *VS = R"(#version 330
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNor;
layout(location = 2) in vec3 iCenter;
layout(location = 3) in vec4 iQuat;
layout(location = 4) in vec4 iColor;
uniform mat4 mvp;
out vec3 vPos;
out vec3 vNor;
out vec3 vCol;
out vec3 vLocal;
out vec3 vLocalN;
vec3 qrot(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
void main() {
    vNor = qrot(iQuat, aNor);
    vPos = iCenter + qrot(iQuat, aPos);
    vCol = iColor.rgb;
    vLocal = aPos + 0.5;
    vLocalN = aNor;
    gl_Position = mvp * vec4(vPos, 1.0);
}
)";
static const char *FS_MAIN = R"(
in vec3 vPos;
in vec3 vNor;
in vec3 vCol;
in vec3 vLocal;
in vec3 vLocalN;
out vec4 fragColor;
void main() {
    vec3 base = blockTexture(vCol, vLocal, vLocalN, 0.0);
    vec3 a = abs(vLocalN);
    vec2 f = a.x > 0.5 ? vLocal.zy : a.y > 0.5 ? vLocal.xz : vLocal.xy;
    float e = min(min(f.x, 1.0 - f.x), min(f.y, 1.0 - f.y));
    if (e < 1.0 / 16.0) base *= 0.82;   /* 结构的方块描边重一点，看得清形状 */
    vec3 c = lighting(base, vPos, normalize(vNor), 1.0, true);
    fragColor = vec4(applyFog(c, vPos, 1.0), 1.0);
}
)";
static const char *FS_DEPTH = R"(#version 330
void main() {}
)";

static Shader g_lit, g_depth;
static unsigned g_vao, g_cube_vbo, g_inst_vbo;
static size_t g_inst_cap;
static std::vector<Instance> g_inst;

static void set_instance_attribs() {
    rlEnableVertexBuffer(g_inst_vbo);
    rlSetVertexAttribute(2, 3, RL_FLOAT, false, sizeof(Instance), 0);
    rlSetVertexAttribute(3, 4, RL_FLOAT, false, sizeof(Instance), 12);
    rlSetVertexAttribute(4, 4, RL_UNSIGNED_BYTE, true, sizeof(Instance), 28);
    for (int i = 2; i <= 4; i++) {
        rlEnableVertexAttribute(i);
        rlSetVertexAttributeDivisor(i, 1);
    }
}

void bodies_init() {
    std::string fs = std::string("#version 330\n") + LIGHT_GLSL + FS_MAIN;
    g_lit = LoadShaderFromMemory(VS, fs.c_str());
    g_depth = LoadShaderFromMemory(VS, FS_DEPTH);
    render_set_common(g_lit);
    /* 边长 1、中心在原点的立方体：36 个顶点（位置 + 法线） */
    static const float F[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
    std::vector<float> v;
    for (auto &n : F) {
        int a = n[0] ? 0 : n[1] ? 1 : 2, b = (a + 1) % 3, c = (a + 2) % 3;
        float s = n[a];
        float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        int order[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k : order) {
            float p[3];
            p[a] = 0.5f * s;
            p[b] = 0.5f * corner[k][0] * s; /* 正反两面绕向相反 */
            p[c] = 0.5f * corner[k][1];
            v.insert(v.end(), { p[0], p[1], p[2], n[0], n[1], n[2] });
        }
    }
    g_vao = rlLoadVertexArray();
    rlEnableVertexArray(g_vao);
    g_cube_vbo = rlLoadVertexBuffer(v.data(), (int)(v.size() * sizeof(float)), false);
    rlSetVertexAttribute(0, 3, RL_FLOAT, false, 24, 0);
    rlSetVertexAttribute(1, 3, RL_FLOAT, false, 24, 12);
    rlEnableVertexAttribute(0);
    rlEnableVertexAttribute(1);
    g_inst_cap = 4096;
    g_inst_vbo = rlLoadVertexBuffer(nullptr, (int)(g_inst_cap * sizeof(Instance)), true);
    set_instance_attribs();
    rlDisableVertexArray();
}

Vector3 g_view_pos;

template <class T> static T rd(const uint8_t *p) { T v; memcpy(&v, p, sizeof v); return v; }

static int64_t bkey(int x, int y, int z) {
    return ((int64_t)(uint16_t)x << 32) | ((int64_t)(uint16_t)y << 16) | (uint16_t)z;
}

bool Bodies::handle(const uint8_t *m, size_t n, double now_ms) {
    switch (m[0]) {
    case 2: { /* 形状 */
        int32_t id = rd<int32_t>(m + 1);
        uint32_t cnt = rd<uint32_t>(m + 5);
        if (n < 9 + (size_t)cnt * 9) return true;
        Body b;
        b.blocks.resize((size_t)cnt * 3);
        b.colors.resize((size_t)cnt * 3);
        b.lookup.reserve(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *e = m + 9 + (size_t)i * 9;
            for (int k = 0; k < 3; k++) {
                b.blocks[i * 3 + k] = rd<int16_t>(e + k * 2);
                b.colors[i * 3 + k] = e[6 + k];
            }
            b.lookup.insert(bkey(b.blocks[i * 3], b.blocks[i * 3 + 1], b.blocks[i * 3 + 2]));
        }
        map[id] = std::move(b);
        return true;
    }
    case 3: { /* 位姿 */
        double t = rd<double>(m + 1);
        uint32_t cnt = rd<uint32_t>(m + 9);
        if (latest_t < 0 || t > latest_t) {
            latest_t = t;
            latest_recv = now_ms;
        }
        for (uint32_t i = 0; i < cnt && 13 + (size_t)(i + 1) * 56 <= n; i++) {
            const uint8_t *e = m + 13 + (size_t)i * 56;
            auto it = map.find(rd<int32_t>(e));
            if (it == map.end()) continue;
            Sample s;
            s.t = t;
            for (int k = 0; k < 3; k++) s.p[k] = rd<double>(e + 4 + k * 8);
            for (int k = 0; k < 4; k++) s.q[k] = rd<float>(e + 28 + k * 4);
            for (int k = 0; k < 3; k++) s.com[k] = rd<float>(e + 44 + k * 4);
            auto &S = it->second.samples;
            if (!S.empty() && t <= S.back().t) continue;
            S.push_back(s);
            if (S.size() > 40) S.erase(S.begin(), S.end() - 40);
        }
        return true;
    }
    case 4:
        map.erase(rd<int32_t>(m + 1));
        return true;
    }
    return false;
}

void Bodies::clear() {
    map.clear();
    latest_t = clock = -1;
}

/* 渲染时钟：落后最新位姿 120ms，慢慢追，抖动不会传到画面上 */
static constexpr double DELAY = 120;

void Bodies::interpolate(double now) {
    if (latest_t < 0) return;
    double target = latest_t + (now - latest_recv) - DELAY;
    if (clock < 0 || fabs(target - clock) > 400) clock = target;
    else clock += (now - last_frame) + (target - clock) * 0.02;
    last_frame = now;
    double rt = clock;
    for (auto &kv : map) {
        Body &b = kv.second;
        auto &S = b.samples;
        if (S.empty()) continue;
        size_t drop = 0;
        while (S.size() - drop > 2 && S[drop + 1].t <= rt) drop++;
        if (drop) S.erase(S.begin(), S.begin() + (long)drop);
        const Sample *a = &S[0], *c = S.size() > 1 ? &S[1] : &S[0];
        float f = 0;
        if (rt <= a->t) c = a;
        else if (rt >= c->t) a = c;
        else f = (float)((rt - a->t) / (c->t - a->t));
        b.pos = Vector3{ (float)(a->p[0] + (c->p[0] - a->p[0]) * f), (float)(a->p[1] + (c->p[1] - a->p[1]) * f),
                         (float)(a->p[2] + (c->p[2] - a->p[2]) * f) };
        b.rot = QuaternionSlerp(Quaternion{ a->q[0], a->q[1], a->q[2], a->q[3] }, Quaternion{ c->q[0], c->q[1], c->q[2], c->q[3] }, f);
        b.com = Vector3{ a->com[0] + (c->com[0] - a->com[0]) * f, a->com[1] + (c->com[1] - a->com[1]) * f,
                         a->com[2] + (c->com[2] - a->com[2]) * f };
        if (!b.visible || b.radius == 0) {
            float r2 = 0;
            for (size_t i = 0; i < b.blocks.size(); i += 3) {
                Vector3 d{ b.blocks[i] + 0.5f - b.com.x, b.blocks[i + 1] + 0.5f - b.com.y, b.blocks[i + 2] + 0.5f - b.com.z };
                r2 = fmaxf(r2, Vector3LengthSqr(d));
            }
            b.radius = sqrtf(r2) + 0.87f;
        }
        b.visible = true;
    }
}

size_t Bodies::block_count() const {
    size_t n = 0;
    for (auto &kv : map) n += kv.second.colors.size() / 3;
    return n;
}

/* 一次实例化绘制画出全部结构的全部方块 */
void Bodies::draw(bool depth_only) {
    g_inst.clear();
    for (auto &kv : map) {
        const Body &b = kv.second;
        if (!b.visible) continue;
        size_t cnt = b.colors.size() / 3;
        for (size_t i = 0; i < cnt; i++) {
            Vector3 l{ b.blocks[i * 3] + 0.5f - b.com.x, b.blocks[i * 3 + 1] + 0.5f - b.com.y, b.blocks[i * 3 + 2] + 0.5f - b.com.z };
            Vector3 w = Vector3Add(b.pos, Vector3RotateByQuaternion(l, b.rot));
            Instance in;
            in.c[0] = w.x; in.c[1] = w.y; in.c[2] = w.z;
            in.q[0] = b.rot.x; in.q[1] = b.rot.y; in.q[2] = b.rot.z; in.q[3] = b.rot.w;
            in.col[0] = b.colors[i * 3]; in.col[1] = b.colors[i * 3 + 1]; in.col[2] = b.colors[i * 3 + 2]; in.col[3] = 255;
            g_inst.push_back(in);
        }
    }
    if (g_inst.empty()) return;
    rlDrawRenderBatchActive();
    rlEnableVertexArray(g_vao);
    if (g_inst.size() > g_inst_cap) {
        while (g_inst_cap < g_inst.size()) g_inst_cap *= 2;
        rlUnloadVertexBuffer(g_inst_vbo);
        g_inst_vbo = rlLoadVertexBuffer(nullptr, (int)(g_inst_cap * sizeof(Instance)), true);
        set_instance_attribs();
    }
    rlUpdateVertexBuffer(g_inst_vbo, g_inst.data(), (int)(g_inst.size() * sizeof(Instance)), 0);
    Shader &sh = depth_only ? g_depth : g_lit;
    rlEnableShader(sh.id);
    rlSetUniformMatrix(GetShaderLocation(sh, "mvp"), MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection()));
    if (!depth_only) {
        Vector3 cam = g_view_pos;
        rlSetUniform(GetShaderLocation(sh, "viewPos"), &cam, RL_SHADER_UNIFORM_VEC3, 1);
        rlSetUniformMatrix(GetShaderLocation(sh, "lightVP"), g_render.light_vp);
        rlSetUniform(GetShaderLocation(sh, "flash"), &g_flash, RL_SHADER_UNIFORM_VEC4, 1);
        int slot = 1;
        rlActiveTextureSlot(1);
        rlEnableTexture(render_shadow_texture());
        rlSetUniform(GetShaderLocation(sh, "texture1"), &slot, RL_SHADER_UNIFORM_INT, 1);
    }
    rlDisableBackfaceCulling();
    rlDrawVertexArrayInstanced(0, 36, (int)g_inst.size());
    rlEnableBackfaceCulling();
    if (!depth_only) {
        rlDisableTexture();
        rlActiveTextureSlot(0);
    }
    rlDisableShader();
    rlDisableVertexArray();
}

/* 射线拾取：先用包围球筛，再在结构自己的坐标里走体素 */
bool Bodies::pick(Vector3 o, Vector3 d, float max_dist, BodyHit *out) const {
    float best = max_dist;
    bool found = false;
    for (auto &kv : map) {
        const Body &b = kv.second;
        if (!b.visible) continue;
        Vector3 rel = Vector3Subtract(b.pos, o);
        float along = Vector3DotProduct(rel, d);
        float perp2 = Vector3LengthSqr(rel) - along * along;
        if (perp2 > b.radius * b.radius || along + b.radius < 0 || along - b.radius > best) continue;
        Quaternion inv = QuaternionInvert(b.rot);
        Vector3 lo = Vector3Add(Vector3RotateByQuaternion(Vector3Subtract(o, b.pos), inv), b.com);
        Vector3 ld = Vector3RotateByQuaternion(d, inv);
        /* 从包围球入口附近开始走 */
        float t0 = fmaxf(0.f, along - b.radius);
        Vector3 s = Vector3Add(lo, Vector3Scale(ld, t0));
        int x = (int)floorf(s.x), y = (int)floorf(s.y), z = (int)floorf(s.z);
        int sx = ld.x > 0 ? 1 : -1, sy = ld.y > 0 ? 1 : -1, sz = ld.z > 0 ? 1 : -1;
        float tdx = fabsf(1 / ld.x), tdy = fabsf(1 / ld.y), tdz = fabsf(1 / ld.z);
        float tx = (ld.x > 0 ? (x + 1 - s.x) : (s.x - x)) * tdx;
        float ty = (ld.y > 0 ? (y + 1 - s.y) : (s.y - y)) * tdy;
        float tz = (ld.z > 0 ? (z + 1 - s.z) : (s.z - z)) * tdz;
        float t = 0, limit = fminf(best, along + b.radius) - t0;
        while (t <= limit) {
            if (b.lookup.count(bkey(x, y, z))) {
                float dist = t0 + t;
                if (dist < best) {
                    best = dist;
                    found = true;
                    out->id = kv.first;
                    out->dist = dist;
                    out->point = Vector3Add(o, Vector3Scale(d, dist));
                }
                break;
            }
            if (tx < ty && tx < tz) { x += sx; t = tx; tx += tdx; }
            else if (ty < tz) { y += sy; t = ty; ty += tdy; }
            else { z += sz; t = tz; tz += tdz; }
        }
    }
    return found;
}

} // namespace mcc::client
