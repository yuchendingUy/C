/*
 * 音效：全部现场合成（没有音频文件），放方块、拆方块、脚步、物理枪、大厅底噪和远处的回响。
 */
#ifndef MCC_CLIENT_SOUND_H
#define MCC_CLIENT_SOUND_H

namespace mcc::client {

enum Sfx { SFX_PLACE, SFX_BREAK, SFX_STEP, SFX_GRAB, SFX_DROP, SFX_FREEZE, SFX_CLICK, SFX_COUNT };

void sound_init();
void sound_close();
void sound_play(Sfx s, float volume = 1, float pitch_jitter = 0.08f);
/* 每帧：环境声（底噪、偶尔远处一声金属响），物理枪抓着时的嗡嗡声 */
void sound_update(float dt, bool gun_hum);

} // namespace mcc::client

#endif
