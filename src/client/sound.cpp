#include "sound.h"
#include "raylib.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>

namespace mcc::client {

static const int RATE = 44100;
static bool g_ok;
static Sound g_sfx[SFX_COUNT];
static Sound g_room, g_hum, g_clank[3];
static float g_clank_wait = 8, g_hum_vol = 0;
static uint32_t g_rng = 12345;

static float frand() {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / (float)0xFFFFFF;
}

static Sound make(const std::vector<float> &v) {
    Wave w = {};
    w.frameCount = (unsigned)v.size();
    w.sampleRate = RATE;
    w.sampleSize = 16;
    w.channels = 1;
    int16_t *d = (int16_t *)MemAlloc((unsigned)(v.size() * 2));
    for (size_t i = 0; i < v.size(); i++) d[i] = (int16_t)(fmaxf(-1, fminf(1, v[i])) * 32000);
    w.data = d;
    Sound s = LoadSoundFromWave(w);
    UnloadWave(w);
    return s;
}

/* 大厅里的回声：一串越来越弱、越来越闷的延迟 */
static std::vector<float> echo(std::vector<float> v, float delay, float decay, int taps) {
    size_t d = (size_t)(delay * RATE), n = v.size();
    v.resize(n + d * taps, 0.f);
    float g = 1, y = 0;
    for (int t = 1; t <= taps; t++) {
        g *= decay;
        for (size_t i = 0; i < n; i++) {
            y += 0.35f * (v[i] - y);
            v[i + d * t] += y * g;
        }
    }
    return v;
}

/* 低通滤过的噪声，k 越小越闷 */
static std::vector<float> noise(float sec, float k) {
    std::vector<float> v((size_t)(sec * RATE));
    float y = 0;
    for (auto &x : v) x = (y += k * ((frand() * 2 - 1) - y));
    return v;
}

void sound_init() {
    InitAudioDevice();
    g_ok = IsAudioDeviceReady();
    if (!g_ok) return;
    SetMasterVolume(0.8f);
    {   /* 放方块：闷的一下 */
        auto v = noise(0.12f, 0.25f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = (float)i / RATE;
            v[i] = (v[i] * 1.6f + sinf(2 * PI * 95 * t) * 0.5f) * expf(-t * 38);
        }
        g_sfx[SFX_PLACE] = make(echo(v, 0.19f, 0.4f, 4));
    }
    {   /* 拆方块：碎的几下 */
        auto v = noise(0.22f, 0.5f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = (float)i / RATE;
            float env = expf(-t * 20) + 0.5f * expf(-fmaxf(0, t - 0.05f) * 30) * (t > 0.05f) + 0.3f * expf(-fmaxf(0, t - 0.1f) * 40) * (t > 0.1f);
            v[i] *= env * 1.4f;
        }
        g_sfx[SFX_BREAK] = make(echo(v, 0.19f, 0.35f, 4));
    }
    {   /* 脚步：混凝土地面上硬的一下，带回声 */
        auto v = noise(0.06f, 0.7f);
        for (size_t i = 0; i < v.size(); i++) {
            float t = (float)i / RATE;
            v[i] = (v[i] * 0.8f + sinf(2 * PI * 140 * t) * 0.4f) * fminf(1, t * 900) * expf(-t * 70);
        }
        g_sfx[SFX_STEP] = make(echo(v, 0.21f, 0.3f, 3));
    }
    auto sweep = [](float sec, float f0, float f1, float amp) {
        std::vector<float> v((size_t)(sec * RATE));
        float ph = 0;
        for (size_t i = 0; i < v.size(); i++) {
            float t = (float)i / RATE, u = t / sec;
            ph += 2 * PI * (f0 + (f1 - f0) * u) / RATE;
            v[i] = (sinf(ph) * 0.7f + sinf(ph * 2.01f) * 0.2f) * amp * fminf(1, t * 300) * (1 - u) * (1 - u);
        }
        return v;
    };
    g_sfx[SFX_GRAB] = make(sweep(0.16f, 300, 950, 0.5f));
    g_sfx[SFX_DROP] = make(sweep(0.18f, 700, 220, 0.45f));
    g_sfx[SFX_FREEZE] = make(sweep(0.25f, 1400, 1300, 0.35f));
    g_sfx[SFX_CLICK] = make(sweep(0.03f, 2000, 1800, 0.3f));
    {   /* 物理枪嗡嗡声：1 秒一圈，首尾相接 */
        std::vector<float> v(RATE);
        for (int i = 0; i < RATE; i++) {
            float t = (float)i / RATE;
            v[i] = (sinf(2 * PI * 110 * t) * 0.5f + sinf(2 * PI * 220 * t) * 0.25f + sinf(2 * PI * 331 * t) * 0.1f) * (0.75f + 0.25f * sinf(2 * PI * 6 * t)) * 0.5f;
        }
        g_hum = make(v);
    }
    {   /* 大厅的底噪：很闷的低频噪声 + 灯管的 50Hz 嗡嗡声，6 秒首尾交叉淡化循环 */
        auto v = noise(6.5f, 0.008f);
        size_t n = 6 * RATE, x = v.size() - n;
        std::vector<float> w(n);
        for (size_t i = 0; i < n; i++) {
            float t = (float)i / RATE;
            float s = v[i];
            if (i < x) {
                float a = (float)i / x;
                s = v[i] * a + v[n + i] * (1 - a);
            }
            w[i] = s * 6 + (sinf(2 * PI * 50 * t) * 0.03f + sinf(2 * PI * 100 * t) * 0.02f) * (0.85f + 0.15f * sinf(2 * PI * t / 3));
        }
        g_room = make(w);
    }
    for (int c = 0; c < 3; c++) {  /* 远处偶尔一声金属响，长长的回声 */
        float sec = 0.5f;
        std::vector<float> v((size_t)(sec * RATE));
        static const float F[3][4] = { { 210, 537, 873, 1311 }, { 160, 402, 691, 1043 }, { 260, 611, 977, 1520 } };
        for (size_t i = 0; i < v.size(); i++) {
            float t = (float)i / RATE;
            float x = 0;
            for (int k = 0; k < 4; k++) x += sinf(2 * PI * F[c][k] * t) * expf(-t * (6 + k * 5)) / (k + 1);
            v[i] = x * 0.4f * fminf(1, t * 2000) + (frand() * 2 - 1) * 0.3f * expf(-t * 80);
        }
        g_clank[c] = make(echo(v, 0.33f, 0.55f, 7));
    }
    SetSoundVolume(g_room, 0.5f);
    PlaySound(g_room);
}

void sound_close() {
    if (!g_ok) return;
    for (auto &s : g_sfx) UnloadSound(s);
    for (auto &s : g_clank) UnloadSound(s);
    UnloadSound(g_room);
    UnloadSound(g_hum);
    CloseAudioDevice();
}

void sound_play(Sfx s, float volume, float jitter) {
    if (!g_ok) return;
    SetSoundVolume(g_sfx[s], volume);
    SetSoundPitch(g_sfx[s], 1 + (frand() * 2 - 1) * jitter);
    PlaySound(g_sfx[s]);
}

void sound_update(float dt, bool hum) {
    if (!g_ok) return;
    if (!IsSoundPlaying(g_room)) PlaySound(g_room);
    g_clank_wait -= dt;
    if (g_clank_wait <= 0) {
        Sound &c = g_clank[g_rng % 3];
        SetSoundVolume(c, 0.05f + frand() * 0.07f);
        SetSoundPitch(c, 0.8f + frand() * 0.3f);
        SetSoundPan(c, frand());
        PlaySound(c);
        g_clank_wait = 15 + frand() * 25;
    }
    g_hum_vol += ((hum ? 0.3f : 0.f) - g_hum_vol) * fminf(1, dt * 10);
    if (g_hum_vol > 0.01f) {
        if (!IsSoundPlaying(g_hum)) PlaySound(g_hum);
        SetSoundVolume(g_hum, g_hum_vol);
    } else if (IsSoundPlaying(g_hum)) StopSound(g_hum);
}

} // namespace mcc::client
