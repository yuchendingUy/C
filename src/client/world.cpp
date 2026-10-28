#include "world.h"
#include "raymath.h"
#include <math.h>
#include <string.h>

namespace mcc::client {

const int DIRV[6][3] = { { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 } };

/* ------------------------------------------------------------------ */
/* 网格拼装：顶点颜色里预先乘上按法线算的明暗                            */
/* ------------------------------------------------------------------ */

struct MeshBuilder {
    std::vector<float> pos, nor;
    std::vector<uint8_t> col;
    std::vector<uint16_t> idx;

    size_t verts() const { return pos.size() / 3; }
    static float shade(Vector3 n) {
        Vector3 l = Vector3Normalize(Vector3{ 0.35f, 1.0f, 0.25f });
        float d = n.x * l.x + n.y * l.y + n.z * l.z;
        return 0.62f + 0.38f * (d > 0 ? d : 0) - (n.y < -0.5f ? 0.12f : 0);
    }
    void vert(Vector3 p, Vector3 n, Color c, float s) {
        pos.insert(pos.end(), { p.x, p.y, p.z });
        nor.insert(nor.end(), { n.x, n.y, n.z });
        col.insert(col.end(), { (uint8_t)(c.r * s), (uint8_t)(c.g * s), (uint8_t)(c.b * s), c.a });
    }
    void quad(Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 n, Color col_, float s) {
        uint16_t base = (uint16_t)verts();
        vert(a, n, col_, s); vert(b, n, col_, s); vert(c, n, col_, s); vert(d, n, col_, s);
        idx.insert(idx.end(), { base, (uint16_t)(base + 1), (uint16_t)(base + 2), base, (uint16_t)(base + 2), (uint16_t)(base + 3) });
    }
    /* 绕 Y 轴转 ang 的长方体 */
    void box(Vector3 c, Vector3 h, Color color, float ang = 0) {
        float cs = cosf(ang), sn = sinf(ang);
        auto rot = [&](Vector3 v) { return Vector3{ v.x * cs + v.z * sn, v.y, -v.x * sn + v.z * cs }; };
        auto P = [&](float x, float y, float z) { return Vector3Add(c, rot(Vector3{ x * h.x, y * h.y, z * h.z })); };
        struct F { Vector3 n; float a[4][3]; } faces[6] = {
            { { -1, 0, 0 }, { { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 }, { -1, -1, -1 } } },
            { { 1, 0, 0 }, { { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 } } },
            { { 0, -1, 0 }, { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } } },
            { { 0, 1, 0 }, { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } } },
            { { 0, 0, -1 }, { { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 }, { 1, -1, -1 } } },
            { { 0, 0, 1 }, { { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 }, { -1, -1, 1 } } },
        };
        for (auto &f : faces) {
            Vector3 n = rot(f.n);
            quad(P(f.a[0][0], f.a[0][1], f.a[0][2]), P(f.a[1][0], f.a[1][1], f.a[1][2]), P(f.a[2][0], f.a[2][1], f.a[2][2]),
                 P(f.a[3][0], f.a[3][1], f.a[3][2]), n, color, shade(n));
        }
    }
    /* 沿 Y 的圆柱 [y0, y1] */
    void cylinder(float r, float y0, float y1, int seg, Color color) {
        for (int i = 0; i < seg; i++) {
            float a0 = (float)i / seg * 2 * PI, a1 = (float)(i + 1) / seg * 2 * PI;
            Vector3 p0{ cosf(a0) * r, 0, sinf(a0) * r }, p1{ cosf(a1) * r, 0, sinf(a1) * r };
            Vector3 n{ cosf((a0 + a1) / 2), 0, sinf((a0 + a1) / 2) };
            quad(Vector3{ p0.x, y0, p0.z }, Vector3{ p0.x, y1, p0.z }, Vector3{ p1.x, y1, p1.z }, Vector3{ p1.x, y0, p1.z }, n, color, shade(n));
            quad(Vector3{ 0, y1, 0 }, Vector3{ 0, y1, 0 }, Vector3{ p1.x, y1, p1.z }, Vector3{ p0.x, y1, p0.z }, Vector3{ 0, 1, 0 }, color,
                 shade(Vector3{ 0, 1, 0 }));
            quad(Vector3{ 0, y0, 0 }, Vector3{ 0, y0, 0 }, Vector3{ p0.x, y0, p0.z }, Vector3{ p1.x, y0, p1.z }, Vector3{ 0, -1, 0 }, color,
                 shade(Vector3{ 0, -1, 0 }));
        }
    }
    Mesh upload() {
        Mesh m = {};
        m.vertexCount = (int)verts();
        m.triangleCount = (int)idx.size() / 3;
        m.vertices = (float *)MemAlloc((unsigned)(pos.size() * sizeof(float)));
        m.normals = (float *)MemAlloc((unsigned)(nor.size() * sizeof(float)));
        m.colors = (unsigned char *)MemAlloc((unsigned)col.size());
        m.indices = (unsigned short *)MemAlloc((unsigned)(idx.size() * sizeof(uint16_t)));
        memcpy(m.vertices, pos.data(), pos.size() * sizeof(float));
        memcpy(m.normals, nor.data(), nor.size() * sizeof(float));
        memcpy(m.colors, col.data(), col.size());
        memcpy(m.indices, idx.data(), idx.size() * sizeof(uint16_t));
        UploadMesh(&m, false);
        return m;
    }
};

/* ------------------------------------------------------------------ */
/* 机械动力零件的网格（都以 Y 为转轴，中心在原点）                       */
/* ------------------------------------------------------------------ */

static Mesh m_shaft, m_shaft_enc, m_half_pos, m_half_neg, m_cog, m_lcog, m_box;
static const Color SHAFT_C{ 181, 181, 174, 255 }, WOOD{ 156, 110, 60, 255 }, WOOD_D{ 122, 85, 48, 255 }, WHITE_C{ 255, 255, 255, 255 };

static Mesh cog_mesh(float radius, int teeth) {
    MeshBuilder b;
    b.cylinder(radius - 0.06f, -0.125f, 0.125f, 24, WOOD);
    for (int i = 0; i < teeth; i++) {
        float a = (float)i / teeth * 2 * PI;
        b.box(Vector3{ cosf(a) * (radius - 0.02f), 0, sinf(a) * (radius - 0.02f) }, Vector3{ 0.07f, 0.11f, 0.07f }, WOOD_D, -a);
    }
    b.cylinder(0.19f, -0.5f, 0.5f, 10, SHAFT_C);
    return b.upload();
}

void world_init_meshes() {
    { MeshBuilder b; b.cylinder(0.19f, -0.5f, 0.5f, 10, SHAFT_C); m_shaft = b.upload(); }
    { MeshBuilder b; b.cylinder(0.2f, -0.53f, 0.53f, 10, SHAFT_C); m_shaft_enc = b.upload(); }
    { MeshBuilder b; b.cylinder(0.19f, 0, 0.53f, 10, SHAFT_C); m_half_pos = b.upload(); }
    { MeshBuilder b; b.cylinder(0.19f, -0.53f, 0, 10, SHAFT_C); m_half_neg = b.upload(); }
    m_cog = cog_mesh(0.45f, 14);
    m_lcog = cog_mesh(0.95f, 28);
    { MeshBuilder b; b.box(Vector3{ 0, 0, 0 }, Vector3{ 0.5f, 0.5f, 0.5f }, WHITE_C); m_box = b.upload(); }
}

/* 把绕 Y 转的零件摆到 axis 上 */
static Matrix orient(int axis) {
    if (axis == 0) return MatrixRotateZ(-PI / 2);
    if (axis == 2) return MatrixRotateX(PI / 2);
    return MatrixIdentity();
}

/* ------------------------------------------------------------------ */
/* 方块读写                                                            */
/* ------------------------------------------------------------------ */

static int cidx(int lx, int y, int lz) { return (y - MINY) * 256 + lz * 16 + lx; }

uint16_t World::get(int32_t x, int32_t y, int32_t z) const {
    if (y < MINY || y >= MINY + HEIGHT) return 0;
    auto it = chunks.find(ckey(x >> 4, z >> 4));
    return it == chunks.end() ? 0 : it->second.data[cidx(x & 15, y, z & 15)];
}

void World::set(int32_t x, int32_t y, int32_t z, uint16_t s) {
    if (y < MINY || y >= MINY + HEIGHT) return;
    auto it = chunks.find(ckey(x >> 4, z >> 4));
    if (it == chunks.end()) return;
    Chunk &c = it->second;
    c.data[cidx(x & 15, y, z & 15)] = s;
    c.dirty = true;
    c.ymin = std::min(c.ymin, y - 1);
    c.ymax = std::max(c.ymax, y + 1);
    static const int N[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    for (auto &d : N) {
        auto n = chunks.find(ckey((x + d[0]) >> 4, (z + d[1]) >> 4));
        if (n != chunks.end() && &n->second != &c) n->second.dirty = true;
    }
    int64_t k = pkey(x, y, z);
    if (s && !air(s) && pal[s].kind != K_CUBE) kinetics[k] = s;
    else {
        kinetics.erase(k);
        speeds.erase(k);
    }
}

const std::string &World::name_of(uint32_t s) const {
    static const std::string none = "?";
    if (names.empty()) return none;
    size_t lo = 0, hi = names.size() - 1;
    while (lo < hi) {
        size_t m = (lo + hi + 1) / 2;
        if (names[m].first <= s) lo = m;
        else hi = m - 1;
    }
    return names[lo].name;
}

const BlockName *World::block_by_name(const std::string &n) const {
    for (auto &b : names)
        if (b.name == n) return &b;
    return nullptr;
}

/* ------------------------------------------------------------------ */
/* 消息                                                                */
/* ------------------------------------------------------------------ */

template <class T> static T rd(const uint8_t *p) { T v; memcpy(&v, p, sizeof v); return v; }

static void free_meshes(std::vector<Mesh> &v) {
    for (Mesh &m : v) UnloadMesh(m);
    v.clear();
}

void World::clear() {
    for (auto &kv : chunks) {
        free_meshes(kv.second.meshes);
        free_meshes(kv.second.water);
    }
    chunks.clear();
    kinetics.clear();
    speeds.clear();
}

bool World::handle(const uint8_t *m, size_t n) {
    switch (m[0]) {
    case 10: { /* 调色板 */
        uint32_t cnt = rd<uint32_t>(m + 1);
        pal.resize(cnt);
        if (n >= 5 + (size_t)cnt * 13) memcpy(pal.data(), m + 5, (size_t)cnt * 13);
        return true;
    }
    case 12: { /* 方块名 */
        uint32_t cnt = rd<uint32_t>(m + 1);
        names.clear();
        size_t o = 5;
        for (uint32_t i = 0; i < cnt && o + 9 <= n; i++) {
            BlockName b;
            b.first = rd<uint32_t>(m + o);
            b.count = rd<uint32_t>(m + o + 4);
            uint8_t len = m[o + 8];
            b.name.assign((const char *)m + o + 9, len);
            names.push_back(std::move(b));
            o += 9 + len;
        }
        return true;
    }
    case 9: { /* 区块全部方块 */
        int32_t cx = rd<int32_t>(m + 1), cz = rd<int32_t>(m + 5);
        Chunk &c = chunks[ckey(cx, cz)];
        free_meshes(c.meshes);
        free_meshes(c.water);
        c = Chunk{};
        c.cx = cx;
        c.cz = cz;
        c.data.assign(256 * HEIGHT, 0);
        size_t o = 9;
        for (int lz = 0; lz < 16; lz++)
            for (int lx = 0; lx < 16; lx++) {
                uint8_t runs = m[o++];
                for (int r = 0; r < runs; r++, o += 6) {
                    int16_t y0 = rd<int16_t>(m + o);
                    uint16_t len = rd<uint16_t>(m + o + 2), s = rd<uint16_t>(m + o + 4);
                    for (int y = y0; y < y0 + len; y++) {
                        c.data[cidx(lx, y, lz)] = s;
                        if (s < pal.size() && pal[s].kind != K_CUBE) kinetics[pkey(cx * 16 + lx, y, cz * 16 + lz)] = s;
                    }
                    c.ymin = std::min(c.ymin, y0 - 1);
                    c.ymax = std::max(c.ymax, y0 + len);
                }
            }
        static const int N[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
        for (auto &d : N) {
            auto it = chunks.find(ckey(cx + d[0], cz + d[1]));
            if (it != chunks.end()) it->second.dirty = true;
        }
        return true;
    }
    case 6: /* 方块变化 */
        set(rd<int32_t>(m + 1), rd<int32_t>(m + 5), rd<int32_t>(m + 9), (uint16_t)rd<uint32_t>(m + 13));
        return true;
    case 8: { /* 动力部件转速 */
        uint32_t cnt = rd<uint32_t>(m + 1);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *e = m + 5 + i * 17;
            speeds[pkey(rd<int32_t>(e), rd<int32_t>(e + 4), rd<int32_t>(e + 8))] = rd<float>(e + 12);
        }
        return true;
    }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* 区块网格                                                            */
/* ------------------------------------------------------------------ */

static const struct { int n[3]; float c[4][3]; float shade; } FACE[6] = {
    { { -1, 0, 0 }, { { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 }, { 0, 0, 0 } }, 0.80f },
    { { 1, 0, 0 }, { { 1, 0, 0 }, { 1, 1, 0 }, { 1, 1, 1 }, { 1, 0, 1 } }, 0.80f },
    { { 0, -1, 0 }, { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 } }, 0.55f },
    { { 0, 1, 0 }, { { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 0 } }, 1.00f },
    { { 0, 0, -1 }, { { 0, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 1, 0, 0 } }, 0.70f },
    { { 0, 0, 1 }, { { 1, 0, 1 }, { 1, 1, 1 }, { 0, 1, 1 }, { 0, 0, 1 } }, 0.70f },
};

static void face(MeshBuilder &b, float x, float y, float z, int f, Color c, const float box[6]) {
    Vector3 v[4];
    for (int k = 0; k < 4; k++) {
        const float *cc = FACE[f].c[k];
        v[k] = Vector3{ x + (cc[0] ? box[3] : box[0]), y + (cc[1] ? box[4] : box[1]), z + (cc[2] ? box[5] : box[2]) };
    }
    b.quad(v[0], v[1], v[2], v[3], Vector3{ (float)FACE[f].n[0], (float)FACE[f].n[1], (float)FACE[f].n[2] }, c, FACE[f].shade);
}

void World::rebuild(int budget) {
    if (!ready()) return;
    for (auto &kv : chunks) {
        if (budget <= 0) break;
        Chunk &c = kv.second;
        if (!c.dirty) continue;
        c.dirty = false;
        budget--;
        free_meshes(c.meshes);
        free_meshes(c.water);
        MeshBuilder solid, water;
        int ox = c.cx * 16, oz = c.cz * 16;
        int y0 = std::max(c.ymin, MINY), y1 = std::min(c.ymax, MINY + HEIGHT - 1);
        for (int y = y0; y <= y1; y++)
            for (int lz = 0; lz < 16; lz++)
                for (int lx = 0; lx < 16; lx++) {
                    uint16_t s = c.data[cidx(lx, y, lz)];
                    if (!s || air(s) || pal[s].kind != K_CUBE) continue;
                    const PalEntry &e = pal[s];
                    int x = ox + lx, z = oz + lz;
                    if (liquid_only(s)) {
                        if (!(pal[get(x, y + 1, z)].flags & F_LIQ)) {
                            static const float wb[6] = { 0, 0, 0, 1, 0.88f, 1 };
                            face(water, (float)x, (float)y, (float)z, 3, Color{ e.r, e.g, e.b, 150 }, wb);
                        }
                    } else {
                        bool f = full(s);
                        float box[6] = { 0, 0, 0, 1, 1, 1 };
                        if (!f) {
                            if (e.box[3] > e.box[0]) for (int k = 0; k < 6; k++) box[k] = e.box[k] / 16.f;
                            else { box[0] = box[2] = 0.3f; box[3] = box[5] = 0.7f; box[4] = 0.8f; }
                        }
                        for (int fi = 0; fi < 6; fi++) {
                            const int *nn = FACE[fi].n;
                            if (f && full(get(x + nn[0], y + nn[1], z + nn[2]))) continue;
                            face(solid, (float)x, (float)y, (float)z, fi, Color{ e.r, e.g, e.b, 255 }, box);
                        }
                    }
                    /* 网格顶点编号是 16 位：快满了就另起一块 */
                    if (solid.verts() > 60000) { c.meshes.push_back(solid.upload()); solid = MeshBuilder(); }
                    if (water.verts() > 60000) { c.water.push_back(water.upload()); water = MeshBuilder(); }
                }
        if (solid.verts()) c.meshes.push_back(solid.upload());
        if (water.verts()) c.water.push_back(water.upload());
    }
}

void World::draw(Material &mat) {
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    for (auto &kv : chunks)
        for (Mesh &m : kv.second.meshes) DrawMesh(m, mat, MatrixIdentity());
}

void World::draw_water(Material &mat) {
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    for (auto &kv : chunks)
        for (Mesh &m : kv.second.water) DrawMesh(m, mat, MatrixIdentity());
}

/* ------------------------------------------------------------------ */
/* 动力零件：按服务端转速转（RPM × 6 = 度/秒）                           */
/* ------------------------------------------------------------------ */

void World::draw_kinetics(Material &mat, double t) {
    for (auto &kv : kinetics) {
        int64_t key = kv.first;
        uint32_t s = kv.second;
        int32_t x = (int32_t)(key >> 38), z = (int32_t)((key << 26) >> 38), y = (int32_t)((key << 52) >> 52);
        const PalEntry &e = pal[s];
        Vector3 c{ x + 0.5f, y + 0.5f, z + 0.5f };
        Matrix T = MatrixTranslate(c.x, c.y, c.z);
        auto speed_at = [&](int64_t k) { auto it = speeds.find(k); return it == speeds.end() ? 0.f : it->second; };
        float own = speed_at(key);
        auto angle = [&](float sp, float off) { return (float)fmod(t * sp * 6.0 + off, 360.0) * DEG2RAD; };
        auto rotor = [&](Mesh &m, int axis, float sp, float off) {
            mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            DrawMesh(m, mat, MatrixMultiply(MatrixMultiply(MatrixRotateY(angle(sp, off)), orient(axis)), T));
        };
        auto casing = [&](float size, Color col, Vector3 off = Vector3{ 0, 0, 0 }) {
            mat.maps[MATERIAL_MAP_DIFFUSE].color = col;
            DrawMesh(m_box, mat, MatrixMultiply(MatrixScale(size, size, size), MatrixTranslate(c.x + off.x, c.y + off.y, c.z + off.z)));
        };
        /* 机械动力的齿轮错位：小齿轮每隔一格错开半个齿 */
        auto cog_off = [&](bool large) {
            int a = e.axis;
            int sum = (a == 0 ? 0 : x) + (a == 1 ? 0 : y) + (a == 2 ? 0 : z);
            return (sum % 2 == 0) ? 22.5f : (large ? 11.25f : 0.f);
        };
        Color col{ e.r, e.g, e.b, 255 };
        switch (e.kind) {
        case K_SHAFT: rotor(m_shaft, e.axis, own, 0); break;
        case K_COG: rotor(m_cog, e.axis, own, cog_off(false)); break;
        case K_LCOG: rotor(m_lcog, e.axis, own, cog_off(true)); break;
        case K_ENC: case K_CHAIN: case K_ENC_COG: case K_ENC_LCOG:
            casing(0.98f, col);
            rotor(m_shaft_enc, e.axis, own, 0);
            break;
        case K_GEARBOX: case K_SPLIT: {
            casing(0.98f, col);
            if (e.kind == K_SPLIT) {
                bool clutch = name_of(s).find("clutch") != std::string::npos;
                Color band = clutch ? Color{ 192, 57, 43, 255 } : Color{ 46, 134, 193, 255 };
                mat.maps[MATERIAL_MAP_DIFFUSE].color = band;
                float sx = e.axis == 0 ? 0.25f : 1.01f, sy = e.axis == 1 ? 0.25f : 1.01f, sz = e.axis == 2 ? 0.25f : 1.01f;
                DrawMesh(m_box, mat, MatrixMultiply(MatrixScale(sx, sy, sz), T));
            }
            /* 每一侧按那一侧连着的部件转速转（齿轮箱各侧转向不同） */
            for (int d = 0; d < 6; d++) {
                bool along = dir_axis(d) == e.axis;
                if ((e.kind == K_GEARBOX) == along) continue;
                int64_t nk = pkey(x + DIRV[d][0], y + DIRV[d][1], z + DIRV[d][2]);
                float sp = speeds.count(nk) ? speeds[nk] : own;
                rotor(dir_positive(d) ? m_half_pos : m_half_neg, dir_axis(d), sp, 0);
            }
            break;
        }
        case K_MOTOR: {
            int f = e.facing;
            casing(0.62f, col, Vector3{ -DIRV[f][0] * 0.12f, -DIRV[f][1] * 0.12f, -DIRV[f][2] * 0.12f });
            rotor(dir_positive(f) ? m_half_pos : m_half_neg, dir_axis(f), own, 0);
            break;
        }
        case K_CTRL: {
            mat.maps[MATERIAL_MAP_DIFFUSE].color = col;
            DrawMesh(m_box, mat, MatrixMultiply(MatrixScale(1, 0.25f, 1), MatrixTranslate(c.x, c.y - 0.375f, c.z)));
            mat.maps[MATERIAL_MAP_DIFFUSE].color = Color{ 138, 122, 90, 255 };
            DrawMesh(m_box, mat, MatrixMultiply(MatrixScale(0.88f, 0.55f, 0.88f), MatrixTranslate(c.x, c.y + 0.05f, c.z)));
            rotor(m_shaft_enc, e.axis, own, 0);
            break;
        }
        }
    }
    mat.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
}

} // namespace mcc::client
