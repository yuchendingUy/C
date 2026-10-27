#include "viewer.h"
#include "../core/log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_OUT (256u << 20) /* 发送缓冲超过 256MB 的客户端直接断开 */

typedef MCC_VEC(uint8_t) bytes;

typedef struct client {
    int fd;
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
    int listen_fd;
    pthread_t thread;
    _Atomic bool stop;
    pthread_mutex_t lock;
    MCC_VEC(client *) clients;
    uint32_t next_client;
    MCC_VEC(uint64_t) chunks;
    mcc_hmap region_clear;      /* 区域编号 → 已处理的清空序号 + 1 */
    _Atomic uint64_t clear_epoch;
    char web_dir[512];
    mcc_viewer_spawn_fn spawn;
    void *ctx;
    uint8_t *colors;            /* 每个方块状态的 RGB */
};

/* ------------------------------------------------------------------ */
/* 小工具：字节缓冲、SHA-1、Base64                                     */
/* ------------------------------------------------------------------ */

static void put(bytes *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n) cap *= 2;
        b->data = mcc_xrealloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void put_u8(bytes *b, uint8_t v) { put(b, &v, 1); }
static void put_i16(bytes *b, int16_t v) { put(b, &v, 2); }
static void put_i32(bytes *b, int32_t v) { put(b, &v, 4); }
static void put_u32(bytes *b, uint32_t v) { put(b, &v, 4); }
static void put_f32(bytes *b, float v) { put(b, &v, 4); }
static void put_f64(bytes *b, double v) { put(b, &v, 8); }

static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t *m = mcc_xcalloc(total, 1);
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
    v->colors = mcc_xmalloc(mcc_blocks.state_count * 3u);
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

/* ------------------------------------------------------------------ */
/* 地形快照（区块任务，区域线程上执行）                                 */
/* ------------------------------------------------------------------ */

typedef struct { mcc_viewer *v; uint32_t client; } terrain_req;

static void task_terrain(mcc_region *r, int32_t cx, int32_t cz, void *arg) {
    terrain_req *q = arg;
    if (r) {
        mcc_region_world *w = r->data;
        const mcc_chunk *c = mcc_store_chunk(w->chunks, cx, cz);
        if (c) {
            bytes b = { 0 };
            put_u8(&b, 1);
            put_i32(&b, cx);
            put_i32(&b, cz);
            size_t count_at = b.len;
            put_u32(&b, 0);
            uint32_t n = 0;
            for (int i = 0; i < c->nsec; i++) {
                const mcc_chunk_section *s = c->sec[i];
                if (!s || !s->phys) continue;
                int32_t by = (c->min_sy + i) << 4;
                for (int y = 0; y < 16; y++)
                    for (int z = 0; z < 16; z++) {
                        uint32_t row = s->phys->solid[y][z] | s->phys->liquid[y][z];
                        while (row) {
                            int x = __builtin_ctz(row);
                            row &= row - 1;
                            mcc_state_t st = mcc_section_get(s, mcc_section_index(x, y, z));
                            bool liquid = s->phys->liquid[y][z] >> x & 1, solid = s->phys->solid[y][z] >> x & 1;
                            /* 流体只发表面（上方不是流体） */
                            if (liquid && !solid) {
                                mcc_state_t up = mcc_store_get(w->chunks, (cx << 4) + x, by + y + 1, (cz << 4) + z);
                                if (mcc_state_pflags(up) & MCC_PHYS_LIQUID) continue;
                            }
                            /* 露在外面的面：实心方块看邻居是不是整块；流体只画顶面 */
                            uint8_t faces = 0;
                            if (liquid && !solid) faces = 1 << 3;
                            else {
                                static const int D[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
                                for (int f = 0; f < 6; f++) {
                                    mcc_state_t nb = mcc_store_get(w->chunks, (cx << 4) + x + D[f][0], by + y + D[f][1], (cz << 4) + z + D[f][2]);
                                    if (!(mcc_state_pflags(nb) & MCC_PHYS_FULL)) faces |= (uint8_t)(1 << f);
                                }
                            }
                            if (!faces) continue;
                            put_u8(&b, (uint8_t)x);
                            put_u8(&b, (uint8_t)z);
                            put_i16(&b, (int16_t)(by + y));
                            put(&b, q->v->colors + st * 3, 3);
                            put_u8(&b, (uint8_t)(faces | (liquid && !solid ? 64 : 0)));
                            n++;
                        }
                    }
            }
            memcpy(b.data + count_at, &n, 4);
            pthread_mutex_lock(&q->v->lock);
            client *cl = find_client(q->v, q->client);
            if (cl) ws_send(cl, b.data, b.len);
            pthread_mutex_unlock(&q->v->lock);
            mcc_vec_free(&b);
        }
    }
    free(q);
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
        const mcc_chunk *c = p;
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

void mcc_viewer_post_tick(void *vp, mcc_region *r, mcc_region_world *w) {
    mcc_viewer *v = vp;
    double now_ms = mcc_nanotime() / 1e6;

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
        mcc_sublevel *sl = p;
        phys_body *pb = phys_body_get(w->phys, sl->id);
        if (!pb || !pb->has_com) continue;
        if (b3Body_IsAwake(pb->body)) {
            encode_pose(sl, pb, &awake);
            na++;
        }
    }
    end_poses(&awake, na);

    pthread_mutex_lock(&v->lock);
    for (size_t k = 0; k < v->clients.len; k++) {
        client *c = v->clients.data[k];
        if (!c->ws || c->dead) continue;
        begin_poses(&extra, now_ms);
        uint32_t ne = 0;
        it = 0;
        while (mcc_hmap_next(&w->sublevels, &it, NULL, &p)) {
            mcc_sublevel *sl = p;
            phys_body *pb = phys_body_get(w->phys, sl->id);
            if (!pb || !pb->has_com || mcc_hmap_get(&c->known, (uint64_t)(uint32_t)sl->id)) continue;
            shape.len = 0;
            encode_shape(v, sl, &shape);
            ws_send(c, shape.data, shape.len);
            mcc_hmap_put(&c->known, (uint64_t)(uint32_t)sl->id, (void *)1);
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
    close(c->fd);
    mcc_vec_free(&c->out);
    mcc_hmap_free(&c->known);
    free(c);
}

static void send_terrain(mcc_viewer *v, uint32_t client_id) {
    for (size_t i = 0; i < v->chunks.len; i++) {
        terrain_req *q = mcc_xmalloc(sizeof *q);
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
    char *key = strcasestr((char *)c->in, "Sec-WebSocket-Key:");
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
    mcc_info("测试客户端已连接（#%u），推送 %zu 个地形区块", c->id, v->chunks.len);
    send_terrain(v, c->id);
}

static void handle_command(mcc_viewer *v, const char *cmd) {
    double x, z;
    int n;
    if (sscanf(cmd, "spawn %lf %lf %d", &x, &z, &n) == 3) {
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
        if (op == 1) handle_command(v, msg);
    }
}

static void *net_main(void *p) {
    mcc_viewer *v = p;
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
        poll(fds.data, (nfds_t)fds.len, 5);

        if (fds.data[0].revents & POLLIN) {
            int fd = accept(v->listen_fd, NULL, NULL);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, O_NONBLOCK);
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                client *c = mcc_xcalloc(1, sizeof *c);
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
                ssize_t n = recv(c->fd, c->in + c->inlen, sizeof c->in - 1 - c->inlen, 0);
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
                ssize_t n = send(c->fd, c->out.data + c->out_off, c->out.len - c->out_off, MSG_NOSIGNAL);
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
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
        /* 回收断开的连接 */
        pthread_mutex_lock(&v->lock);
        size_t w = 0;
        for (size_t i = 0; i < v->clients.len; i++) {
            client *c = v->clients.data[i];
            if (c->dead) {
                if (c->ws) mcc_info("测试客户端断开（#%u）", c->id);
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
    mcc_viewer *v = mcc_xcalloc(1, sizeof *v);
    v->lv = lv;
    v->spawn = spawn;
    v->ctx = ctx;
    snprintf(v->web_dir, sizeof v->web_dir, "%s", web_dir);
    pthread_mutex_init(&v->lock, NULL);
    mcc_hmap_init(&v->region_clear, 16);
    build_colors(v);
    v->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(v->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(v->listen_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(v->listen_fd, 16) < 0)
        mcc_log_fatal("测试客户端服务：端口 %d 监听失败：%s", port, strerror(errno));
    fcntl(v->listen_fd, F_SETFL, O_NONBLOCK);
    pthread_create(&v->thread, NULL, net_main, v);
    mcc_info("测试客户端：浏览器打开 http://<本机地址>:%d/", port);
    return v;
}

void mcc_viewer_stop(mcc_viewer *v) {
    atomic_store(&v->stop, true);
    pthread_join(v->thread, NULL);
    close(v->listen_fd);
    for (size_t i = 0; i < v->clients.len; i++) client_free(v->clients.data[i]);
    mcc_vec_free(&v->clients);
    mcc_vec_free(&v->chunks);
    mcc_hmap_free(&v->region_clear);
    free(v->colors);
    pthread_mutex_destroy(&v->lock);
    free(v);
}

void mcc_viewer_add_chunk(mcc_viewer *v, int32_t cx, int32_t cz) {
    pthread_mutex_lock(&v->lock);
    mcc_vec_push(&v->chunks, mcc_key2(cx, cz));
    pthread_mutex_unlock(&v->lock);
}
