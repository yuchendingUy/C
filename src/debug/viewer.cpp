#include "viewer.h"
#include "../core/log.h"
#include "../game/game.h"
#include "../game/pending.h"
#include "../create/kinetics/blocks.h"
#include "../phys/colliders.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

/* 网络：Linux 用 BSD 套接字，Windows 用 Winsock */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define sock_close closesocket
#define sock_poll WSAPoll
#define SEND_FLAGS 0
static bool would_block(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }
static void set_nonblock(sock_t fd) { u_long one = 1; ioctlsocket(fd, FIONBIO, &one); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define sock_close close
#define sock_poll poll
#define SEND_FLAGS MSG_NOSIGNAL
static bool would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK; }
static void set_nonblock(sock_t fd) { fcntl(fd, F_SETFL, O_NONBLOCK); }
#endif

/* 不区分大小写查找子串（Windows 没有 strcasestr） */
static char *find_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == n) return (char *)hay;
    }
    return NULL;
}

#define MAX_OUT (256u << 20) /* 发送缓冲超过 256MB 的客户端直接断开 */

typedef MCC_VEC(uint8_t) bytes;

typedef struct client {
    sock_t fd;
    uint32_t id;
    bool ws, close_after_flush, dead;
    uint8_t in[16384];
    size_t inlen;
    bytes out;           /* v->lock 保护 */
    size_t out_off;
    mcc_hmap known;      /* 已发过形状的结构编号（v->lock 保护） */
} client;

struct mcc_viewer {
    mcc_level *lv;
    sock_t listen_fd;
    pthread_t thread;
    _Atomic(bool) stop;
    pthread_mutex_t lock;
    MCC_VEC(client *) clients;
    uint32_t next_client;
    MCC_VEC(uint64_t) chunks;
    mcc_hmap region_clear;      /* 区域编号 → 已处理的清空序号 + 1 */
    mcc_hmap region_stats;      /* 区域编号 → region_stat*（推进统计） */
    double stats_at;            /* 上次发统计的时间（毫秒） */
    _Atomic(uint64_t) clear_epoch;
    char web_dir[512];
    mcc_viewer_spawn_fn spawn;
    void *ctx;
    uint8_t *colors;            /* 每个方块状态的 RGB */
    bytes palette;              /* 调色板消息（启动时生成一次） */
    bytes names;                /* 方块名表消息 */
    /* 待发的动力部件转速（推进时收集，每刻发一次）：坐标键 → kin 下标 + 1 */
    mcc_hmap kin_index;
    MCC_VEC(struct kin_update) kin;
    /* 物理枪：每个客户端最多抓一个结构 */
    MCC_VEC(struct grab) grabs;
};

typedef struct kin_update {
    int32_t x, y, z;
    float speed;
    uint8_t flags;              /* 1 超载 */
} kin_update;

typedef struct grab {
    uint32_t client;
    int32_t body;
    bool has_local;             /* 抓取点已换算成结构局部坐标 */
    double hit[3];              /* 抓住时的世界坐标 */
    double local[3];            /* 抓取点（相对质心，结构局部系） */
    double target[3];           /* 抓取点要去的位置 */
} grab;

static mcc_viewer *g_viewer;    /* 网络钩子用（调试服务端只有一个） */

/* ------------------------------------------------------------------ */
/* 小工具：字节缓冲、SHA-1、Base64                                     */
/* ------------------------------------------------------------------ */

static void put(bytes *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n) cap *= 2;
        b->data = (uint8_t*)mcc_xrealloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void put_u8(bytes *b, uint8_t v) { put(b, &v, 1); }
static void put_i16(bytes *b, int16_t v) { put(b, &v, 2); }
static void put_u16(bytes *b, uint16_t v) { put(b, &v, 2); }
static void put_i32(bytes *b, int32_t v) { put(b, &v, 4); }
static void put_u32(bytes *b, uint32_t v) { put(b, &v, 4); }
static void put_f32(bytes *b, float v) { put(b, &v, 4); }
static void put_f64(bytes *b, double v) { put(b, &v, 8); }

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t *m = (uint8_t*)mcc_xcalloc(total, 1);
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 | (uint32_t)m[off + 4 * i + 2] << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t t = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = t << 1 | t >> 31;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
            e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

static void base64(const uint8_t *in, size_t n, char *out) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* 方块颜色（按名字粗略取色，只为看得清）                               */
/* ------------------------------------------------------------------ */

static void build_colors(mcc_viewer *v) {
    static const struct { const char *key; uint8_t r, g, b; } T[] = {
        { "water", 50, 110, 220 }, { "lava", 230, 100, 20 }, { "grass_block", 95, 160, 65 }, { "dirt", 125, 90, 60 },
        { "sand", 220, 205, 150 }, { "gravel", 135, 130, 125 }, { "deepslate", 75, 75, 82 }, { "stone", 128, 128, 128 },
        { "cobblestone", 115, 115, 115 }, { "planks", 175, 140, 85 }, { "log", 110, 85, 50 }, { "leaves", 60, 120, 40 },
        { "glass", 190, 220, 230 }, { "wool", 230, 230, 230 }, { "iron", 210, 210, 210 }, { "gold", 240, 200, 60 },
        { "ice", 160, 200, 250 }, { "snow", 245, 250, 250 }, { "bedrock", 50, 50, 50 },
    };
    v->colors = (uint8_t*)mcc_xmalloc(mcc_blocks.state_count * 3u);
    for (uint32_t s = 0; s < mcc_blocks.state_count; s++) {
        const char *name = mcc_state_def(s)->name;
        uint8_t *c = v->colors + s * 3;
        uint64_t h = mcc_hash64(mcc_state_block(s) + 1);
        c[0] = (uint8_t)(90 + (h & 127));
        c[1] = (uint8_t)(90 + (h >> 8 & 127));
        c[2] = (uint8_t)(90 + (h >> 16 & 127));
        for (size_t i = 0; i < MCC_ARRAY_LEN(T); i++)
            if (strstr(name, T[i].key)) {
                c[0] = T[i].r; c[1] = T[i].g; c[2] = T[i].b;
                break;
            }
    }
}

/* ------------------------------------------------------------------ */
/* 调色板：客户端画方块、算碰撞、认机械动力部件用                       */
/* ------------------------------------------------------------------ */

/* 渲染种类（客户端按它画转动的部件） */
enum {
    K_CUBE = 0, K_SHAFT, K_COG, K_LARGE_COG, K_ENCASED_SHAFT, K_GEARBOX, K_SPLIT, K_CHAIN, K_MOTOR, K_CONTROLLER,
    K_ENCASED_COG, K_ENCASED_LARGE_COG,
};

static void create_colors(mcc_viewer *v) {
    static const struct { const char *name; uint8_t r, g, b; } T[] = {
        { "create:shaft", 175, 175, 170 }, { "create:cogwheel", 160, 115, 65 }, { "create:large_cogwheel", 150, 105, 60 },
        { "create:andesite_encased_shaft", 160, 155, 140 }, { "create:brass_encased_shaft", 205, 160, 75 },
        { "create:andesite_encased_cogwheel", 160, 155, 140 }, { "create:brass_encased_cogwheel", 205, 160, 75 },
        { "create:andesite_encased_large_cogwheel", 160, 155, 140 }, { "create:brass_encased_large_cogwheel", 205, 160, 75 },
        { "create:gearbox", 160, 155, 140 }, { "create:clutch", 160, 155, 140 }, { "create:gearshift", 160, 155, 140 },
        { "create:encased_chain_drive", 160, 155, 140 }, { "create:adjustable_chain_gearshift", 200, 165, 90 },
        { "create:creative_motor", 165, 80, 200 }, { "create:rotation_speed_controller", 205, 160, 75 },
    };
    for (size_t i = 0; i < MCC_ARRAY_LEN(T); i++) {
        int32_t b = mcc_block_by_name(T[i].name);
        if (b < 0) continue;
        const mcc_block_def *d = &mcc_blocks.blocks[b];
        for (uint32_t s = 0; s < d->state_count; s++) {
            uint8_t *c = v->colors + (d->first_state + s) * 3;
            c[0] = T[i].r; c[1] = T[i].g; c[2] = T[i].b;
        }
    }
}

static uint8_t render_kind(mcc_state_t s) {
    static const struct { const char *name; uint8_t kind; } T[] = {
        { "create:shaft", K_SHAFT }, { "create:cogwheel", K_COG }, { "create:large_cogwheel", K_LARGE_COG },
        { "create:andesite_encased_shaft", K_ENCASED_SHAFT }, { "create:brass_encased_shaft", K_ENCASED_SHAFT },
        { "create:andesite_encased_cogwheel", K_ENCASED_COG }, { "create:brass_encased_cogwheel", K_ENCASED_COG },
        { "create:andesite_encased_large_cogwheel", K_ENCASED_LARGE_COG },
        { "create:brass_encased_large_cogwheel", K_ENCASED_LARGE_COG },
        { "create:gearbox", K_GEARBOX }, { "create:clutch", K_SPLIT }, { "create:gearshift", K_SPLIT },
        { "create:encased_chain_drive", K_CHAIN }, { "create:adjustable_chain_gearshift", K_CHAIN },
        { "create:creative_motor", K_MOTOR }, { "create:rotation_speed_controller", K_CONTROLLER },
    };
    const char *name = mcc_state_def(s)->name;
    for (size_t i = 0; i < MCC_ARRAY_LEN(T); i++)
        if (!strcmp(name, T[i].name)) return T[i].kind;
    return K_CUBE;
}

/* 消息 10：u32 n，n × { rgb, 标志, 种类, 轴, 朝向, 碰撞外框 6 字节（0~16） }
 *   标志：1 空气，2 整块，4 流体，8 有碰撞 */
static void build_palette(mcc_viewer *v) {
    using namespace mcc::create;
    bytes *b = &v->palette;
    put_u8(b, 10);
    put_u32(b, mcc_blocks.state_count);
    for (uint32_t s = 0; s < mcc_blocks.state_count; s++) {
        put(b, v->colors + s * 3, 3);
        uint8_t pf = mcc_state_pflags(s);
        uint8_t flags = (uint8_t)((mcc_state_is_air(s) ? 1 : 0) | (pf & MCC_PHYS_FULL ? 2 : 0) |
                                  (mcc_state_fluid(s) ? 4 : 0) | (pf & MCC_PHYS_SOLID ? 8 : 0));
        put_u8(b, flags);
        uint8_t kind = render_kind(s), axis = 0, facing = 0;
        if (KineticBlock *kb = kinetic_block(s)) axis = (uint8_t)kb->rotation_axis(s);
        if (kind == K_MOTOR) facing = (uint8_t)((CreativeMotorBlock &)mcc::state_behaviour(s)).facing_of(s);
        put_u8(b, kind);
        put_u8(b, axis);
        put_u8(b, facing);
        float lo[3] = { 1, 1, 1 }, hi[3] = { 0, 0, 0 };
        const phys_collider *col = phys_collider_get(mcc_state_collider(s));
        if (col && !col->fluid)
            for (int i = 0; i < col->nboxes; i++)
                for (int k = 0; k < 3; k++) {
                    lo[k] = MCC_MIN(lo[k], col->boxes[i].min[k]);
                    hi[k] = MCC_MAX(hi[k], col->boxes[i].max[k]);
                }
        for (int k = 0; k < 3; k++) put_u8(b, hi[k] > lo[k] ? (uint8_t)lroundf(lo[k] * 16) : 0);
        for (int k = 0; k < 3; k++) put_u8(b, hi[k] > lo[k] ? (uint8_t)lroundf(hi[k] * 16) : 0);
    }
    /* 消息 12：方块名表 u32 n，n × { u32 首状态, u32 状态数, u8 长度, 名字 } */
    bytes *nb = &v->names;
    put_u8(nb, 12);
    put_u32(nb, mcc_blocks.block_count);
    for (uint32_t i = 0; i < mcc_blocks.block_count; i++) {
        const mcc_block_def *d = &mcc_blocks.blocks[i];
        put_u32(nb, d->first_state);
        put_u32(nb, d->state_count);
        size_t n = MCC_MIN(strlen(d->name), (size_t)255);
        put_u8(nb, (uint8_t)n);
        put(nb, d->name, n);
    }
}

/* ------------------------------------------------------------------ */
/* 向客户端发数据（调用方持 v->lock）                                  */
/* ------------------------------------------------------------------ */

static void ws_send(client *c, const uint8_t *payload, size_t n) {
    if (c->dead || !c->ws) return;
    if (c->out.len - c->out_off > MAX_OUT) { c->dead = true; return; }
    uint8_t h[10];
    size_t hl;
    h[0] = 0x82; /* FIN + 二进制 */
    if (n < 126) { h[1] = (uint8_t)n; hl = 2; }
    else if (n < 65536) { h[1] = 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hl = 4; }
    else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (56 - 8 * i)); hl = 10; }
    put(&c->out, h, hl);
    put(&c->out, payload, n);
}

static client *find_client(mcc_viewer *v, uint32_t id) {
    for (size_t i = 0; i < v->clients.len; i++)
        if (v->clients.data[i]->id == id && !v->clients.data[i]->dead) return v->clients.data[i];
    return NULL;
}

typedef struct region_stat {
    uint32_t ticks;      /* 本统计周期内推进的刻数 */
    uint32_t timed;      /* 其中有耗时记录的刻数 */
    uint64_t sum_ns;     /* 这些刻的实际耗时之和 */
    uint64_t max_ns;     /* 其中最慢一刻 */
    uint32_t awake, total; /* 最近一刻醒着的 / 全部结构数 */
    double last;         /* 最近一次推进的时间（毫秒） */
} region_stat;

/* ------------------------------------------------------------------ */
/* 地形快照（区块任务，区域线程上执行）                                 */
/* ------------------------------------------------------------------ */

typedef struct { mcc_viewer *v; uint32_t client; } terrain_req;

/* 消息 8：动力部件转速 u32 n，n × { i32 x, y, z, f32 转速, u8 标志（1 超载） } */
static void put_kin(bytes *b, int32_t x, int32_t y, int32_t z, float speed, uint8_t flags) {
    put_i32(b, x);
    put_i32(b, y);
    put_i32(b, z);
    put_f32(b, speed);
    put_u8(b, flags);
}

/* 消息 9：一个区块的全部方块（按列压缩）：i32 cx, i32 cz，
 * 然后 256 列（z 外层、x 内层），每列 u8 段数，每段 { i16 起始 y, u16 长度, u16 状态 }（只发非空气） */
static void task_terrain(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    terrain_req *q = (terrain_req*)arg;
    if (r) {
        mcc_region_world *w = (mcc_region_world*)r->data;
        mcc_chunk *c = mcc_store_chunk(w->chunks, cx, cz);
        if (c) {
            bytes b = { 0 };
            put_u8(&b, 9);
            put_i32(&b, cx);
            put_i32(&b, cz);
            int32_t y0 = c->min_sy << 4, y1 = (c->min_sy + c->nsec) << 4;
            for (int lz = 0; lz < 16; lz++)
                for (int lx = 0; lx < 16; lx++) {
                    size_t count_at = b.len;
                    put_u8(&b, 0);
                    uint8_t runs = 0;
                    mcc_state_t cur = 0;
                    int32_t start = 0;
                    for (int32_t y = y0; y <= y1; y++) {
                        mcc_state_t st = y < y1 ? mcc_chunk_get(c, lx, y, lz) : 0;
                        if (st == cur && y < y1) continue;
                        if (cur && runs < 255) {
                            put_i16(&b, (int16_t)start);
                            put_u16(&b, (uint16_t)(y - start));
                            put_u16(&b, (uint16_t)cur);
                            runs++;
                        }
                        cur = st;
                        start = y;
                    }
                    b.data[count_at] = runs;
                }
            /* 区块里动力部件的当前转速 */
            bytes k = { 0 };
            put_u8(&k, 8);
            put_u32(&k, 0);
            uint32_t nk = 0;
            if (c->ext) {
                uint32_t it = 0;
                void *val;
                while (mcc_hmap_next(&((mcc::ChunkData *)c->ext)->bes, &it, NULL, &val)) {
                    mcc::create::KineticBlockEntity *kbe = mcc::create::as_kinetic((mcc::BlockEntity *)val);
                    if (!kbe) continue;
                    put_kin(&k, kbe->pos.x, kbe->pos.y, kbe->pos.z, kbe->get_speed(), kbe->over_stressed ? 1 : 0);
                    nk++;
                }
            }
            memcpy(k.data + 1, &nk, 4);
            pthread_mutex_lock(&q->v->lock);
            client *cl = find_client(q->v, q->client);
            if (cl) {
                ws_send(cl, b.data, b.len);
                if (nk) ws_send(cl, k.data, k.len);
            }
            pthread_mutex_unlock(&q->v->lock);
            mcc_vec_free(&b);
            mcc_vec_free(&k);
        }
    }
    free(q);
}

/* ------------------------------------------------------------------ */
/* 游戏层的网络钩子（区域线程上调用）                                   */
/* ------------------------------------------------------------------ */

/* 消息 6：方块变化 i32 x, y, z, u32 状态（只发地形，物理结构上的方块变化暂不同步） */
static void hook_block_updated(mcc::Space &sp, mcc::BlockPos p, mcc_state_t old, mcc_state_t now, int flags) {
    (void)old; (void)flags;
    mcc_viewer *v = g_viewer;
    if (!v || sp.sublevel != 0) return;
    uint8_t msg[17] = { 6 };
    memcpy(msg + 1, &p.x, 4);
    memcpy(msg + 5, &p.y, 4);
    memcpy(msg + 9, &p.z, 4);
    uint32_t s32 = now;
    memcpy(msg + 13, &s32, 4);
    pthread_mutex_lock(&v->lock);
    for (size_t i = 0; i < v->clients.len; i++) ws_send(v->clients.data[i], msg, sizeof msg);
    pthread_mutex_unlock(&v->lock);
}

/* 机械动力 sendData：动力部件的转速变了，攒到本刻结束一起发 */
static void hook_block_entity_data(mcc::BlockEntity *be) {
    mcc_viewer *v = g_viewer;
    mcc::create::KineticBlockEntity *k = mcc::create::as_kinetic(be);
    if (!v || !k || !be->home || be->home->sublevel != 0) return;
    uint64_t key = (uint64_t)be->pos.as_long();
    kin_update u = { be->pos.x, be->pos.y, be->pos.z, be->removed ? 0.f : k->get_speed(), (uint8_t)(k->over_stressed ? 1 : 0) };
    pthread_mutex_lock(&v->lock);
    uintptr_t i = (uintptr_t)mcc_hmap_get(&v->kin_index, key);
    if (i) v->kin.data[i - 1] = u;
    else {
        mcc_vec_push(&v->kin, u);
        mcc_hmap_put(&v->kin_index, key, (void *)(uintptr_t)v->kin.len);
    }
    pthread_mutex_unlock(&v->lock);
}

static void flush_kinetics(mcc_viewer *v) {
    pthread_mutex_lock(&v->lock);
    if (v->kin.len) {
        bytes k = { 0 };
        put_u8(&k, 8);
        put_u32(&k, (uint32_t)v->kin.len);
        for (size_t i = 0; i < v->kin.len; i++) {
            kin_update *u = &v->kin.data[i];
            put_kin(&k, u->x, u->y, u->z, u->speed, u->flags);
        }
        for (size_t i = 0; i < v->clients.len; i++) ws_send(v->clients.data[i], k.data, k.len);
        mcc_vec_free(&k);
        v->kin.len = 0;
        mcc_hmap_free(&v->kin_index);
        mcc_hmap_init(&v->kin_index, 64);
    }
    pthread_mutex_unlock(&v->lock);
}

/* ------------------------------------------------------------------ */
/* 结构形状与位姿（推进后回调，区域线程上执行）                         */
/* ------------------------------------------------------------------ */

static void encode_shape(mcc_viewer *v, const mcc_sublevel *sl, bytes *b) {
    put_u8(b, 2);
    put_i32(b, sl->id);
    size_t count_at = b->len;
    put_u32(b, 0);
    uint32_t n = 0;
    int32_t ox = sl->plot_cx << 4, oz = sl->plot_cz << 4;
    uint32_t it = 0;
    void *p;
    while (mcc_hmap_next(&sl->blocks.chunks, &it, NULL, &p)) {
        const mcc_chunk *c = (const mcc_chunk*)p;
        for (int i = 0; i < c->nsec; i++) {
            const mcc_chunk_section *s = c->sec[i];
            if (!s || !s->non_air) continue;
            for (int idx = 0; idx < MCC_SECTION_BLOCKS; idx++) {
                mcc_state_t st = mcc_section_get(s, idx);
                if (mcc_state_is_air(st)) continue;
                put_i16(b, (int16_t)((c->cx << 4) + (idx & 15) - ox));
                put_i16(b, (int16_t)(((c->min_sy + i) << 4) + (idx >> 8)));
                put_i16(b, (int16_t)((c->cz << 4) + ((idx >> 4) & 15) - oz));
                put(b, v->colors + st * 3, 3);
                n++;
            }
        }
    }
    memcpy(b->data + count_at, &n, 4);
}

static void encode_pose(const mcc_sublevel *sl, const phys_body *pb, bytes *b) {
    b3WorldTransform xf = b3Body_GetTransform(pb->body);
    put_i32(b, sl->id);
    put_f64(b, xf.p.x);
    put_f64(b, xf.p.y);
    put_f64(b, xf.p.z);
    put_f32(b, xf.q.v.x);
    put_f32(b, xf.q.v.y);
    put_f32(b, xf.q.v.z);
    put_f32(b, xf.q.s);
    put_f32(b, (float)(pb->com[0] - (sl->plot_cx << 4)));
    put_f32(b, (float)pb->com[1]);
    put_f32(b, (float)(pb->com[2] - (sl->plot_cz << 4)));
}

static void begin_poses(bytes *b, double ms) {
    b->len = 0;
    put_u8(b, 3);
    put_f64(b, ms);
    put_u32(b, 0);
}

static void end_poses(bytes *b, uint32_t n) { memcpy(b->data + 9, &n, 4); }

/* 物理枪：推进物理之前，把被抓住的结构往目标位置拉（区域线程上，只管本区域里的结构） */
void mcc_viewer_pre_tick(void *vp, mcc_region *r, mcc_region_world *w) {
    (void)r;
    mcc_viewer *v = (mcc_viewer*)vp;
    if (!w->phys) return;
    pthread_mutex_lock(&v->lock);
    size_t n = v->grabs.len;
    grab *list = n ? (grab *)mcc_xmalloc(sizeof(grab) * n) : NULL;
    if (n) memcpy(list, v->grabs.data, sizeof(grab) * n);
    pthread_mutex_unlock(&v->lock);
    for (size_t i = 0; i < n; i++) {
        grab *g = &list[i];
        if (!phys_body_get(w->phys, g->body) || phys_body_frozen(w->phys, g->body)) continue;
        double pose[7];
        phys_body_get_pose(w->phys, g->body, pose);
        const double *q = pose + 3; /* x y z w */
        if (!g->has_local) {
            /* 世界点 → 结构局部：q⁻¹ (p - 原点) */
            double d[3] = { g->hit[0] - pose[0], g->hit[1] - pose[1], g->hit[2] - pose[2] };
            double u[3] = { -q[0], -q[1], -q[2] }, s = q[3];
            double t[3] = { 2 * (u[1] * d[2] - u[2] * d[1]), 2 * (u[2] * d[0] - u[0] * d[2]), 2 * (u[0] * d[1] - u[1] * d[0]) };
            for (int k = 0; k < 3; k++) g->local[k] = d[k] + s * t[k];
            g->local[0] += u[1] * t[2] - u[2] * t[1];
            g->local[1] += u[2] * t[0] - u[0] * t[2];
            g->local[2] += u[0] * t[1] - u[1] * t[0];
            g->has_local = true;
            pthread_mutex_lock(&v->lock);
            for (size_t j = 0; j < v->grabs.len; j++)
                if (v->grabs.data[j].client == g->client && v->grabs.data[j].body == g->body) {
                    memcpy(v->grabs.data[j].local, g->local, sizeof g->local);
                    v->grabs.data[j].has_local = true;
                }
            pthread_mutex_unlock(&v->lock);
        }
        /* 局部点 → 世界：p + q l q⁻¹ */
        const double *l = g->local;
        double t[3] = { 2 * (q[1] * l[2] - q[2] * l[1]), 2 * (q[2] * l[0] - q[0] * l[2]), 2 * (q[0] * l[1] - q[1] * l[0]) };
        double wp[3] = { pose[0] + l[0] + q[3] * t[0] + q[1] * t[2] - q[2] * t[1],
                         pose[1] + l[1] + q[3] * t[1] + q[2] * t[0] - q[0] * t[2],
                         pose[2] + l[2] + q[3] * t[2] + q[0] * t[1] - q[1] * t[0] };
        /* 抓取点按比例追目标，速度上限 60 格/秒；转动阻尼掉，拿着的时候姿态基本不变 */
        double lin[3], sp = 0;
        for (int k = 0; k < 3; k++) {
            lin[k] = (g->target[k] - wp[k]) * 14.0;
            sp += lin[k] * lin[k];
        }
        sp = sqrt(sp);
        if (sp > 60) for (int k = 0; k < 3; k++) lin[k] *= 60 / sp;
        double ang[3];
        phys_body_get_angular_velocity(w->phys, g->body, ang);
        for (int k = 0; k < 3; k++) ang[k] *= 0.6;
        phys_body_set_velocities(w->phys, g->body, lin, ang);
    }
    free(list);
}

void mcc_viewer_post_tick(void *vp, mcc_region *r, mcc_region_world *w) {
    mcc_viewer *v = (mcc_viewer*)vp;
    double now_ms = mcc_nanotime() / 1e6;
    flush_kinetics(v);

    /* 推进统计：网络线程每秒汇总一次 */
    pthread_mutex_lock(&v->lock);
    region_stat *rs = (region_stat*)mcc_hmap_get(&v->region_stats, r->id);
    if (!rs) {
        rs = (region_stat*)mcc_xcalloc(1, sizeof *rs);
        mcc_hmap_put(&v->region_stats, r->id, rs);
    }
    /* 回调在推进里面，本刻还没结束：记上一刻的实际耗时（调度器量的，含本回调） */
    rs->ticks++;
    if (r->last_tick_ns) {
        rs->timed++;
        rs->sum_ns += r->last_tick_ns;
        rs->max_ns = MCC_MAX(rs->max_ns, r->last_tick_ns);
    }
    rs->awake = 0;
    rs->total = (uint32_t)w->sublevels.count;
    rs->last = now_ms;
    pthread_mutex_unlock(&v->lock);

    /* 清空请求：本区域还没处理过的就把结构全删掉 */
    uint64_t epoch = atomic_load(&v->clear_epoch);
    pthread_mutex_lock(&v->lock);
    uintptr_t done = (uintptr_t)mcc_hmap_get(&v->region_clear, r->id);
    if (epoch && done != epoch + 1) mcc_hmap_put(&v->region_clear, r->id, (void *)(uintptr_t)(epoch + 1));
    pthread_mutex_unlock(&v->lock);
    if (epoch && done != epoch + 1 && w->sublevels.count) {
        MCC_VEC(int32_t) ids = { 0 };
        uint32_t it = 0;
        void *p;
        while (mcc_hmap_next(&w->sublevels, &it, NULL, &p)) mcc_vec_push(&ids, ((mcc_sublevel *)p)->id);
        pthread_mutex_lock(&v->lock);
        for (size_t i = 0; i < ids.len; i++) {
            uint8_t msg[5] = { 4 };
            memcpy(msg + 1, &ids.data[i], 4);
            for (size_t k = 0; k < v->clients.len; k++) {
                ws_send(v->clients.data[k], msg, 5);
                mcc_hmap_remove(&v->clients.data[k]->known, (uint64_t)(uint32_t)ids.data[i]);
            }
        }
        pthread_mutex_unlock(&v->lock);
        for (size_t i = 0; i < ids.len; i++) mcc_region_world_remove_sublevel(w, ids.data[i]);
        mcc_vec_free(&ids);
    }
    if (!w->phys || !w->sublevels.count) return;

    /* 醒着的结构：位姿发给所有客户端；客户端还不认识的结构：先发形状再补一个位姿 */
    bytes awake = { 0 }, extra = { 0 }, shape = { 0 };
    begin_poses(&awake, now_ms);
    uint32_t na = 0;
    uint32_t it = 0;
    void *p;
    while (mcc_hmap_next(&w->sublevels, &it, NULL, &p)) {
        mcc_sublevel *sl = (mcc_sublevel*)p;
        phys_body *pb = phys_body_get(w->phys, sl->id);
        if (!pb || !pb->has_com) continue;
        if (b3Body_IsAwake(pb->body)) {
            encode_pose(sl, pb, &awake);
            na++;
        }
    }
    end_poses(&awake, na);

    pthread_mutex_lock(&v->lock);
    region_stat *rs2 = (region_stat*)mcc_hmap_get(&v->region_stats, r->id);
    if (rs2) rs2->awake = na;
    for (size_t k = 0; k < v->clients.len; k++) {
        client *c = v->clients.data[k];
        if (!c->ws || c->dead) continue;
        begin_poses(&extra, now_ms);
        uint32_t ne = 0;
        it = 0;
        while (mcc_hmap_next(&w->sublevels, &it, NULL, &p)) {
            mcc_sublevel *sl = (mcc_sublevel*)p;
            phys_body *pb = phys_body_get(w->phys, sl->id);
            if (!pb || !pb->has_com || mcc_hmap_get(&c->known, (uint64_t)(uint32_t)sl->id)) continue;
            shape.len = 0;
            encode_shape(v, sl, &shape);
            ws_send(c, shape.data, shape.len);
            /* 值不能是 1：哈希表用 (void *)1 标记已删除的槽 */
            mcc_hmap_put(&c->known, (uint64_t)(uint32_t)sl->id, (void *)c);
            encode_pose(sl, pb, &extra);
            ne++;
        }
        if (ne) {
            end_poses(&extra, ne);
            ws_send(c, extra.data, extra.len);
        }
        if (na) ws_send(c, awake.data, awake.len);
    }
    pthread_mutex_unlock(&v->lock);
    mcc_vec_free(&awake);
    mcc_vec_free(&extra);
    mcc_vec_free(&shape);
}

/* ------------------------------------------------------------------ */
/* 网络线程                                                            */
/* ------------------------------------------------------------------ */

static void client_free(client *c) {
    sock_close(c->fd);
    mcc_vec_free(&c->out);
    mcc_hmap_free(&c->known);
    free(c);
}

static void send_terrain(mcc_viewer *v, uint32_t client_id) {
    for (size_t i = 0; i < v->chunks.len; i++) {
        terrain_req *q = (terrain_req*)mcc_xmalloc(sizeof *q);
        q->v = v;
        q->client = client_id;
        if (!mcc_regionizer_queue_chunk_task(&v->lv->rz, mcc_key2_x(v->chunks.data[i]), mcc_key2_z(v->chunks.data[i]), task_terrain, q))
            free(q);
    }
}

static void serve_file(mcc_viewer *v, client *c, const char *path) {
    if (!strcmp(path, "/")) path = "/index.html";
    const char *type = strstr(path, ".js") ? "text/javascript" : "text/html; charset=utf-8";
    char full[1024];
    bool bad = strstr(path, "..") != NULL;
    snprintf(full, sizeof full, "%s%s", v->web_dir, path);
    FILE *f = bad ? NULL : fopen(full, "rb");
    char head[256];
    pthread_mutex_lock(&v->lock);
    if (!f) {
        int n = snprintf(head, sizeof head, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        put(&c->out, head, (size_t)n);
    } else {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        int n = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", type, len);
        put(&c->out, head, (size_t)n);
        mcc_vec_reserve(&c->out, c->out.len + (size_t)len);
        size_t got = fread(c->out.data + c->out.len, 1, (size_t)len, f);
        c->out.len += got;
        fclose(f);
    }
    c->close_after_flush = true;
    pthread_mutex_unlock(&v->lock);
}

/* 解析 HTTP 请求：普通请求发文件，WebSocket 升级则握手并推送地形 */
static void handle_http(mcc_viewer *v, client *c) {
    c->in[MCC_MIN(c->inlen, sizeof c->in - 1)] = 0;
    char *end = strstr((char *)c->in, "\r\n\r\n");
    if (!end) return;
    char path[256] = "/";
    sscanf((char *)c->in, "GET %255s", path);
    char *key = find_ci((char *)c->in, "Sec-WebSocket-Key:");
    if (!key) {
        serve_file(v, c, path);
        c->inlen = 0;
        return;
    }
    key += 18;
    while (*key == ' ') key++;
    char k[128];
    int kl = 0;
    while (key[kl] && key[kl] != '\r' && kl < 100) { k[kl] = key[kl]; kl++; }
    k[kl] = 0;
    strcat(k, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    uint8_t dig[20];
    sha1((uint8_t *)k, strlen(k), dig);
    char acc[64];
    base64(dig, 20, acc);
    char resp[256];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acc);
    size_t used = (size_t)(end + 4 - (char *)c->in);
    memmove(c->in, c->in + used, c->inlen - used);
    c->inlen -= used;
    pthread_mutex_lock(&v->lock);
    put(&c->out, resp, (size_t)n);
    c->ws = true;
    pthread_mutex_unlock(&v->lock);
    pthread_mutex_lock(&v->lock);
    ws_send(c, v->palette.data, v->palette.len);
    ws_send(c, v->names.data, v->names.len);
    pthread_mutex_unlock(&v->lock);
    mcc_info("测试客户端已连接（#%u），推送 %zu 个地形区块", c->id, v->chunks.len);
    send_terrain(v, c->id);
}

typedef struct { int32_t id; double p[3], dv[3]; } push_req;

/* 拍打：在拥有结构的区域线程上施加冲量 */
static void task_push(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    (void)cx;
    (void)cz;
    push_req *q = (push_req*)arg;
    if (r) {
        mcc_region_world *w = (mcc_region_world*)r->data;
        phys_body *b = w->phys ? phys_body_get(w->phys, q->id) : NULL;
        if (b) {
            double m = b3Body_GetMass(b->body), sp = sqrt(q->dv[0] * q->dv[0] + q->dv[1] * q->dv[1] + q->dv[2] * q->dv[2]);
            double k = sp > 40 ? 40 / sp : 1; /* 速度变化上限 40 格/秒 */
            double imp[3] = { q->dv[0] * m * k, q->dv[1] * m * k, q->dv[2] * m * k };
            phys_body_apply_world_impulse(w->phys, q->id, q->p, imp);
        }
    }
    free(q);
}

/* ---- 方块操作：在拥有该位置的区域线程上执行 ---- */

typedef struct {
    mcc_viewer *v;
    uint32_t client;
    int op;                  /* 0 放置，1 破坏，2 调转速，3 查询，4 冻结/解冻 */
    int32_t x, y, z;
    mcc_state_t state;       /* 放置 */
    int delta;               /* 调转速 */
    int32_t body;            /* 冻结 */
} block_req;

static void task_block(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    (void)cx; (void)cz;
    using namespace mcc::create;
    block_req *q = (block_req *)arg;
    if (!r) { free(q); return; }
    mcc_region_world *w = (mcc_region_world *)r->data;
    mcc::Space &sp = mcc::region_space(w);
    mcc::BlockPos p{ q->x, q->y, q->z };
    switch (q->op) {
    case 0: {
        mcc_state_t cur = sp.get(p);
        if (mcc_state_is_air(cur) || (mcc_state_fluid(cur) && !(mcc_state_pflags(cur) & MCC_PHYS_SOLID)))
            sp.set_block(p, q->state);
        break;
    }
    case 1:
        sp.destroy_block(p, true);
        break;
    case 2: {
        mcc::BlockEntity *be = sp.block_entity(p);
        if (be && mcc_state_block(be->state) == g_kblocks.creative_motor) {
            auto *m = (CreativeMotorBlockEntity *)be;
            m->set_generated_speed(m->generated.value + q->delta);
        } else if (be && mcc_state_block(be->state) == g_kblocks.rotation_speed_controller) {
            auto *c = (SpeedControllerBlockEntity *)be;
            c->set_target_speed(c->target.value + q->delta);
        }
        break;
    }
    case 3: {
        /* 消息 11：查询结果 */
        bytes b = { 0 };
        put_u8(&b, 11);
        put_i32(&b, p.x);
        put_i32(&b, p.y);
        put_i32(&b, p.z);
        put_u32(&b, sp.get(p));
        KineticBlockEntity *k = as_kinetic(sp.block_entity(p));
        put_u8(&b, k ? 1 : 0);
        if (k) {
            KineticNetwork *n = k->has_network ? network_get(sp, k->network) : nullptr;
            put_f32(&b, k->get_speed());
            put_u8(&b, k->over_stressed ? 1 : 0);
            put_f32(&b, n ? n->current_stress : 0);
            put_f32(&b, n ? n->current_capacity : 0);
            put_i32(&b, n ? n->size() : 0);
            put_u8(&b, k->has_source ? 1 : 0);
            put_i32(&b, k->source.x);
            put_i32(&b, k->source.y);
            put_i32(&b, k->source.z);
            int value = 0;
            if (mcc_state_block(k->state) == g_kblocks.creative_motor) value = ((CreativeMotorBlockEntity *)k)->generated.value;
            else if (mcc_state_block(k->state) == g_kblocks.rotation_speed_controller) value = ((SpeedControllerBlockEntity *)k)->target.value;
            put_i32(&b, value);
        }
        pthread_mutex_lock(&q->v->lock);
        client *c = find_client(q->v, q->client);
        if (c) ws_send(c, b.data, b.len);
        pthread_mutex_unlock(&q->v->lock);
        mcc_vec_free(&b);
        break;
    }
    case 4:
        if (w->phys && phys_body_get(w->phys, q->body)) phys_body_set_frozen(w->phys, q->body, !phys_body_frozen(w->phys, q->body));
        break;
    }
    free(q);
}

static void queue_block(mcc_viewer *v, block_req *q) {
    if (!mcc_regionizer_queue_chunk_task(&v->lv->rz, q->x >> 4, q->z >> 4, task_block, q)) free(q);
}

/* 放置命令里的方块状态："名字 属性=值 ..." */
static bool parse_state(const char *s, mcc_state_t *out) {
    char name[128];
    int n = 0;
    if (sscanf(s, "%127s%n", name, &n) != 1) return false;
    int32_t b = mcc_block_by_name(name);
    if (b < 0) return false;
    mcc_state_t st = mcc_blocks.blocks[b].default_state;
    s += n;
    char kv[64];
    while (sscanf(s, "%63s%n", kv, &n) == 1) {
        s += n;
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        st = mcc_state_with_str(st, kv, eq + 1);
    }
    *out = st;
    return true;
}

static void handle_command(mcc_viewer *v, client *cl, const char *cmd) {
    double x, z;
    int n;
    push_req pr;
    block_req br = {};
    br.v = v;
    br.client = cl->id;
    int off = 0;
    int32_t id;
    double p3[3];
    if (sscanf(cmd, "push %d %lf %lf %lf %lf %lf %lf", &pr.id, &pr.p[0], &pr.p[1], &pr.p[2], &pr.dv[0], &pr.dv[1], &pr.dv[2]) == 7) {
        push_req *q = (push_req*)mcc_xmalloc(sizeof *q);
        *q = pr;
        if (!mcc_regionizer_queue_chunk_task(&v->lv->rz, (int32_t)floor(pr.p[0]) >> 4, (int32_t)floor(pr.p[2]) >> 4, task_push, q)) free(q);
    } else if (sscanf(cmd, "place %d %d %d %n", &br.x, &br.y, &br.z, &off) == 3 && off) {
        if (!parse_state(cmd + off, &br.state)) return;
        br.op = 0;
        queue_block(v, (block_req *)memcpy(mcc_xmalloc(sizeof br), &br, sizeof br));
    } else if (sscanf(cmd, "break %d %d %d", &br.x, &br.y, &br.z) == 3) {
        br.op = 1;
        queue_block(v, (block_req *)memcpy(mcc_xmalloc(sizeof br), &br, sizeof br));
    } else if (sscanf(cmd, "adjust %d %d %d %d", &br.x, &br.y, &br.z, &br.delta) == 4) {
        br.op = 2;
        queue_block(v, (block_req *)memcpy(mcc_xmalloc(sizeof br), &br, sizeof br));
    } else if (sscanf(cmd, "info %d %d %d", &br.x, &br.y, &br.z) == 3) {
        br.op = 3;
        queue_block(v, (block_req *)memcpy(mcc_xmalloc(sizeof br), &br, sizeof br));
    } else if (sscanf(cmd, "freeze %d %lf %lf %lf", &br.body, &p3[0], &p3[1], &p3[2]) == 4) {
        br.op = 4;
        br.x = (int32_t)floor(p3[0]);
        br.y = (int32_t)floor(p3[1]);
        br.z = (int32_t)floor(p3[2]);
        queue_block(v, (block_req *)memcpy(mcc_xmalloc(sizeof br), &br, sizeof br));
    } else if (sscanf(cmd, "grab %d %lf %lf %lf", &id, &p3[0], &p3[1], &p3[2]) == 4) {
        /* 物理枪抓住：每个客户端只抓一个 */
        pthread_mutex_lock(&v->lock);
        size_t w = 0;
        for (size_t i = 0; i < v->grabs.len; i++)
            if (v->grabs.data[i].client != cl->id) v->grabs.data[w++] = v->grabs.data[i];
        v->grabs.len = w;
        grab g = {};
        g.client = cl->id;
        g.body = id;
        memcpy(g.hit, p3, sizeof p3);
        memcpy(g.target, p3, sizeof p3);
        mcc_vec_push(&v->grabs, g);
        pthread_mutex_unlock(&v->lock);
    } else if (sscanf(cmd, "hold %lf %lf %lf", &p3[0], &p3[1], &p3[2]) == 3) {
        pthread_mutex_lock(&v->lock);
        for (size_t i = 0; i < v->grabs.len; i++)
            if (v->grabs.data[i].client == cl->id) memcpy(v->grabs.data[i].target, p3, sizeof p3);
        pthread_mutex_unlock(&v->lock);
    } else if (!strncmp(cmd, "release", 7)) {
        pthread_mutex_lock(&v->lock);
        size_t w = 0;
        for (size_t i = 0; i < v->grabs.len; i++)
            if (v->grabs.data[i].client != cl->id) v->grabs.data[w++] = v->grabs.data[i];
        v->grabs.len = w;
        pthread_mutex_unlock(&v->lock);
    } else if (sscanf(cmd, "spawn %lf %lf %d", &x, &z, &n) == 3) {
        if (n < 1) n = 1;
        if (n > 5000) n = 5000;
        v->spawn(v->ctx, x, z, n);
    } else if (!strncmp(cmd, "clear", 5)) {
        atomic_fetch_add(&v->clear_epoch, 1);
    }
}

/* 解析客户端发来的 WebSocket 帧（客户端帧带掩码） */
static void handle_ws(mcc_viewer *v, client *c) {
    for (;;) {
        if (c->inlen < 2) return;
        uint8_t op = c->in[0] & 15;
        uint64_t len = c->in[1] & 127;
        size_t pos = 2;
        if (len == 126) {
            if (c->inlen < 4) return;
            len = (uint64_t)c->in[2] << 8 | c->in[3];
            pos = 4;
        } else if (len == 127) {
            c->dead = true;
            return;
        }
        if (c->inlen < pos + 4 + len) return;
        const uint8_t *mask = c->in + pos;
        char msg[1024];
        size_t ml = MCC_MIN((size_t)len, sizeof msg - 1);
        for (size_t i = 0; i < ml; i++) msg[i] = (char)(c->in[pos + 4 + i] ^ mask[i & 3]);
        msg[ml] = 0;
        size_t used = pos + 4 + (size_t)len;
        memmove(c->in, c->in + used, c->inlen - used);
        c->inlen -= used;
        if (op == 8) { c->dead = true; return; }
        if (op == 1) handle_command(v, c, msg);
    }
}

static void *net_main(void *p) {
    mcc_viewer *v = (mcc_viewer*)p;
    MCC_VEC(struct pollfd) fds = { 0 };
    while (!atomic_load(&v->stop)) {
        fds.len = 0;
        mcc_vec_push(&fds, ((struct pollfd){ v->listen_fd, POLLIN, 0 }));
        pthread_mutex_lock(&v->lock);
        for (size_t i = 0; i < v->clients.len; i++) {
            client *c = v->clients.data[i];
            short ev = POLLIN | (c->out.len > c->out_off ? POLLOUT : 0);
            mcc_vec_push(&fds, ((struct pollfd){ c->fd, ev, 0 }));
        }
        pthread_mutex_unlock(&v->lock);
        sock_poll(fds.data, (unsigned)fds.len, 5);

        if (fds.data[0].revents & POLLIN) {
            sock_t fd = accept(v->listen_fd, NULL, NULL);
#ifdef _WIN32
            if (fd != INVALID_SOCKET) {
#else
            if (fd >= 0) {
#endif
                set_nonblock(fd);
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
                client *c = (client*)mcc_xcalloc(1, sizeof *c);
                c->fd = fd;
                mcc_hmap_init(&c->known, 64);
                pthread_mutex_lock(&v->lock);
                c->id = ++v->next_client;
                mcc_vec_push(&v->clients, c);
                pthread_mutex_unlock(&v->lock);
            }
        }
        /* fds[1..] 与 clients 一一对应（新连接在末尾，本轮不处理） */
        for (size_t i = 1; i < fds.len; i++) {
            pthread_mutex_lock(&v->lock);
            client *c = v->clients.data[i - 1];
            pthread_mutex_unlock(&v->lock);
            if (fds.data[i].revents & (POLLERR | POLLHUP)) c->dead = true;
            if (!c->dead && (fds.data[i].revents & POLLIN)) {
                long n = (long)recv(c->fd, (char *)c->in + c->inlen, (int)(sizeof c->in - 1 - c->inlen), 0);
                if (n <= 0) c->dead = true;
                else {
                    c->inlen += (size_t)n;
                    if (!c->ws) handle_http(v, c);
                    if (c->ws) handle_ws(v, c);
                    if (c->inlen >= sizeof c->in - 1) c->dead = true;
                }
            }
            pthread_mutex_lock(&v->lock);
            while (!c->dead && c->out.len > c->out_off) {
                long n = (long)send(c->fd, (const char *)c->out.data + c->out_off, (int)(c->out.len - c->out_off), SEND_FLAGS);
                if (n <= 0) {
                    if (n < 0 && would_block()) break;
                    c->dead = true;
                    break;
                }
                c->out_off += (size_t)n;
            }
            if (c->out_off == c->out.len) {
                c->out.len = c->out_off = 0;
                if (c->close_after_flush) c->dead = true;
            } else if (c->out_off > (1u << 20)) {
                memmove(c->out.data, c->out.data + c->out_off, c->out.len - c->out_off);
                c->out.len -= c->out_off;
                c->out_off = 0;
            }
            pthread_mutex_unlock(&v->lock);
        }
        /* 每秒向所有客户端推送推进统计 */
        double now = mcc_nanotime() / 1e6;
        pthread_mutex_lock(&v->lock);
        if (now - v->stats_at >= 1000) {
            double el = (now - v->stats_at) / 1000.0;
            if (v->stats_at == 0) el = 1;
            v->stats_at = now;
            float min_tps = 0, sum_tps = 0;
            uint64_t sum_ns = 0, max_ns = 0, timed = 0;
            uint32_t nr = 0, awake = 0, total = 0;
            uint32_t it = 0;
            uint64_t key;
            void *val;
            while (mcc_hmap_next(&v->region_stats, &it, &key, &val)) {
                region_stat *rs = (region_stat*)val;
                if (now - rs->last > 3000) { /* 区域已合并或销毁 */
                    mcc_hmap_remove(&v->region_stats, key);
                    free(rs);
                    continue;
                }
                float tps = (float)(rs->ticks / el);
                min_tps = nr ? MCC_MIN(min_tps, tps) : tps;
                sum_tps += tps;
                sum_ns += rs->sum_ns;
                timed += rs->timed;
                max_ns = MCC_MAX(max_ns, rs->max_ns);
                awake += rs->awake;
                total += rs->total;
                rs->ticks = rs->timed = 0;
                rs->sum_ns = rs->max_ns = 0;
                nr++;
            }
            /* 平均：这一秒内所有区域所有刻的平均耗时；最慢：这一秒内最慢的单独一刻 */
            uint8_t msg[29] = { 5 };
            float vals[4] = { min_tps, nr ? sum_tps / nr : 0, timed ? (float)(sum_ns / (double)timed / 1e6) : 0,
                              (float)(max_ns / 1e6) };
            uint32_t cnt[3] = { nr, awake, total };
            memcpy(msg + 1, vals, 16);
            memcpy(msg + 17, cnt, 12);
            for (size_t i = 0; i < v->clients.len; i++) ws_send(v->clients.data[i], msg, sizeof msg);
        }
        /* 回收断开的连接 */
        size_t w = 0;
        for (size_t i = 0; i < v->clients.len; i++) {
            client *c = v->clients.data[i];
            if (c->dead) {
                if (c->ws) mcc_info("测试客户端断开（#%u）", c->id);
                size_t gw = 0;
                for (size_t j = 0; j < v->grabs.len; j++)
                    if (v->grabs.data[j].client != c->id) v->grabs.data[gw++] = v->grabs.data[j];
                v->grabs.len = gw;
                client_free(c);
            } else {
                v->clients.data[w++] = c;
            }
        }
        v->clients.len = w;
        pthread_mutex_unlock(&v->lock);
    }
    mcc_vec_free(&fds);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 启动与停止                                                          */
/* ------------------------------------------------------------------ */

mcc_viewer *mcc_viewer_start(mcc_level *lv, int port, const char *web_dir, mcc_viewer_spawn_fn spawn, void *ctx) {
    mcc_viewer *v = (mcc_viewer*)mcc_xcalloc(1, sizeof *v);
    v->lv = lv;
    v->spawn = spawn;
    v->ctx = ctx;
    snprintf(v->web_dir, sizeof v->web_dir, "%s", web_dir);
    pthread_mutex_init(&v->lock, NULL);
    mcc_hmap_init(&v->region_clear, 16);
    mcc_hmap_init(&v->region_stats, 16);
    mcc_hmap_init(&v->kin_index, 64);
    build_colors(v);
    create_colors(v);
    build_palette(v);
    g_viewer = v;
    mcc::g_net_hooks.block_updated = hook_block_updated;
    mcc::g_net_hooks.block_entity_data = hook_block_entity_data;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    v->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(v->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(v->listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(v->listen_fd, 16) < 0)
        mcc_log_fatal("测试客户端服务：端口 %d 监听失败：%s", port, strerror(errno));
    set_nonblock(v->listen_fd);
    pthread_create(&v->thread, NULL, net_main, v);
    mcc_info("测试客户端：浏览器打开 http://<本机地址>:%d/", port);
    return v;
}

void mcc_viewer_stop(mcc_viewer *v) {
    atomic_store(&v->stop, true);
    pthread_join(v->thread, NULL);
    sock_close(v->listen_fd);
    for (size_t i = 0; i < v->clients.len; i++) client_free(v->clients.data[i]);
    mcc_vec_free(&v->clients);
    mcc_vec_free(&v->chunks);
    mcc_hmap_free(&v->region_clear);
    {
        uint32_t it = 0;
        void *val;
        while (mcc_hmap_next(&v->region_stats, &it, NULL, &val)) free(val);
        mcc_hmap_free(&v->region_stats);
    }
    free(v->colors);
    mcc::g_net_hooks = {};
    g_viewer = NULL;
    mcc_vec_free(&v->palette);
    mcc_vec_free(&v->names);
    mcc_vec_free(&v->kin);
    mcc_vec_free(&v->grabs);
    mcc_hmap_free(&v->kin_index);
    pthread_mutex_destroy(&v->lock);
    free(v);
}

void mcc_viewer_add_chunk(mcc_viewer *v, int32_t cx, int32_t cz) {
    pthread_mutex_lock(&v->lock);
    mcc_vec_push(&v->chunks, mcc_key2(cx, cz));
    pthread_mutex_unlock(&v->lock);
}
