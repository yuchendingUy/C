// 房间里的小收音机
//  1. 你上传的歌（放在服务器 data/music/ 里）
//  2. 没有上传的话，就现场生成轻音乐：王道进行 IV–V–iii–vi + 五声音阶旋律 + 轻轻的鼓
;(function () {
  const NOTE = (n) => 440 * Math.pow(2, (n - 69) / 12) // MIDI → 频率

  // 和弦（MIDI 音高）：F△7  G7  Em7  Am7 | Dm7  G7  C△7  A7sus
  const PROG = [
    { root: 41, notes: [53, 57, 60, 64] },
    { root: 43, notes: [55, 59, 62, 65] },
    { root: 40, notes: [52, 55, 59, 62] },
    { root: 45, notes: [57, 60, 64, 67] },
    { root: 38, notes: [50, 53, 57, 60] },
    { root: 43, notes: [55, 59, 62, 65] },
    { root: 36, notes: [52, 55, 59, 64] },
    { root: 45, notes: [55, 57, 62, 64] },
  ]
  const PENTA = [72, 74, 76, 79, 81, 84, 86, 88] // C 大调五声，高八度
  const RHYTHMS = [
    [0, 1, 1.5, 2.5, 3],
    [0, 0.5, 1, 2, 3, 3.5],
    [0.5, 1, 1.5, 2, 3],
    [0, 1.5, 2, 2.5],
    [0, 1, 2, 2.5, 3, 3.5],
    [0, 2, 2.5, 3],
  ]

  function rng(seed) {
    return () => {
      seed = (seed * 1664525 + 1013904223) >>> 0
      return seed / 4294967296
    }
  }

  class SodaSong {
    constructor(ctx, out) {
      this.ctx = ctx
      this.out = out
      this.bpm = 84
      this.beat = 60 / this.bpm
      this.timer = null
      this.noise = this.makeNoise()
    }
    makeNoise() {
      const b = this.ctx.createBuffer(1, this.ctx.sampleRate * 0.5, this.ctx.sampleRate)
      const d = b.getChannelData(0)
      for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1
      return b
    }
    newPhrase() {
      const r = rng((Math.random() * 1e9) | 0)
      this.phrase = PROG.map((ch) => {
        const rh = RHYTHMS[Math.floor(r() * RHYTHMS.length)]
        let idx = Math.floor(r() * PENTA.length)
        return rh.map((t) => {
          idx = Math.max(0, Math.min(PENTA.length - 1, idx + Math.round((r() - 0.5) * 3)))
          let n = PENTA[idx]
          // 落在强拍上时往和弦音靠
          if (t % 1 === 0 && !ch.notes.some((c) => (c - n) % 12 === 0) && r() < 0.6) {
            n = ch.notes.map((c) => c + 24).reduce((a, c) => (Math.abs(c - n) < Math.abs(a - n) ? c : a))
          }
          return { t, n, len: r() < 0.3 ? 1 : 0.5 }
        })
      })
      this.phraseLeft = 2
    }
    start() {
      if (this.timer) return
      this.newPhrase()
      this.bar = 0
      this.next = this.ctx.currentTime + 0.1
      this.timer = setInterval(() => this.schedule(), 50)
    }
    stop() {
      clearInterval(this.timer)
      this.timer = null
    }
    schedule() {
      while (this.next < this.ctx.currentTime + 0.4) {
        this.playBar(this.bar % 8, this.next)
        this.next += this.beat * 4
        this.bar++
        if (this.bar % 8 === 0 && --this.phraseLeft <= 0) this.newPhrase()
      }
    }
    env(gain, t, a, peak, d, sustain = 0.0001) {
      gain.gain.setValueAtTime(0.0001, t)
      gain.gain.exponentialRampToValueAtTime(peak, t + a)
      gain.gain.exponentialRampToValueAtTime(sustain, t + a + d)
    }
    keys(notes, t, len) {
      for (const n of notes) {
        const o = this.ctx.createOscillator()
        const o2 = this.ctx.createOscillator()
        const g = this.ctx.createGain()
        const f = this.ctx.createBiquadFilter()
        o.type = 'triangle'
        o2.type = 'sine'
        o.frequency.value = NOTE(n)
        o2.frequency.value = NOTE(n) * 2.003
        f.type = 'lowpass'
        f.frequency.value = 1900
        o.connect(f); o2.connect(f); f.connect(g); g.connect(this.out)
        this.env(g, t, 0.015, 0.045, len)
        o.start(t); o2.start(t); o.stop(t + len + 0.1); o2.stop(t + len + 0.1)
      }
    }
    bell(n, t, len) {
      const o = this.ctx.createOscillator()
      const m = this.ctx.createOscillator()
      const mg = this.ctx.createGain()
      const g = this.ctx.createGain()
      o.type = 'sine'
      o.frequency.value = NOTE(n)
      m.frequency.value = NOTE(n) * 3.5
      mg.gain.value = NOTE(n) * 0.6
      m.connect(mg); mg.connect(o.frequency)
      o.connect(g); g.connect(this.out)
      this.env(g, t, 0.006, 0.09, len * this.beat * 1.6)
      o.start(t); m.start(t); o.stop(t + 2); m.stop(t + 2)
    }
    bass(n, t, len) {
      const o = this.ctx.createOscillator()
      const g = this.ctx.createGain()
      o.type = 'sine'
      o.frequency.value = NOTE(n)
      o.connect(g); g.connect(this.out)
      g.gain.setValueAtTime(0.0001, t)
      g.gain.exponentialRampToValueAtTime(0.16, t + 0.02)
      g.gain.exponentialRampToValueAtTime(0.0001, t + len)
      o.start(t); o.stop(t + len + 0.05)
    }
    kick(t) {
      const o = this.ctx.createOscillator()
      const g = this.ctx.createGain()
      o.frequency.setValueAtTime(130, t)
      o.frequency.exponentialRampToValueAtTime(45, t + 0.12)
      o.connect(g); g.connect(this.out)
      g.gain.setValueAtTime(0.22, t)
      g.gain.exponentialRampToValueAtTime(0.0001, t + 0.22)
      o.start(t); o.stop(t + 0.25)
    }
    hat(t, v = 0.025, len = 0.04, freq = 8000) {
      const s = this.ctx.createBufferSource()
      const f = this.ctx.createBiquadFilter()
      const g = this.ctx.createGain()
      s.buffer = this.noise
      f.type = 'highpass'
      f.frequency.value = freq
      s.connect(f); f.connect(g); g.connect(this.out)
      g.gain.setValueAtTime(v, t)
      g.gain.exponentialRampToValueAtTime(0.0001, t + len)
      s.start(t); s.stop(t + len + 0.02)
    }
    playBar(i, t) {
      const b = this.beat
      const ch = PROG[i]
      const swing = b * 0.08
      this.keys(ch.notes, t, b * 1.6)
      this.keys(ch.notes, t + b * 2 + swing, b * 1.2)
      this.bass(ch.root, t, b * 1.4)
      this.bass(ch.root + (i % 2 ? 7 : 12), t + b * 2.5 + swing, b * 0.9)
      this.kick(t)
      this.kick(t + b * 2.5 + swing)
      for (let k = 0; k < 4; k++) {
        this.hat(t + k * b + b * 0.5 + swing)
        if (k % 2) this.hat(t + k * b, 0.04, 0.12, 2500)
      }
      for (const n of this.phrase[i]) this.bell(n.n, t + n.t * b + (n.t % 1 ? swing : 0), n.len)
    }
  }

  class Radio {
    constructor() {
      this.tracks = [] // 服务器上的歌
      this.index = -1 // -1 = 现场小曲
      this.playing = false
      this.audio = new Audio()
      this.audio.preload = 'none'
      this.audio.addEventListener('ended', () => this.next())
      this.audio.addEventListener('error', () => { if (this.index >= 0) this.next() })
      this.volume = 0.35
      try {
        const v = parseFloat(localStorage.getItem('yz-volume'))
        if (v >= 0 && v <= 1) this.volume = v
      } catch {}
      this.onchange = () => {}
    }
    ensureCtx() {
      if (this.ctx) return
      const AC = window.AudioContext || window.webkitAudioContext
      this.ctx = new AC()
      this.master = this.ctx.createGain()
      this.master.gain.value = this.volume * 0.9
      const comp = this.ctx.createDynamicsCompressor()
      this.master.connect(comp)
      comp.connect(this.ctx.destination)
      this.song = new SodaSong(this.ctx, this.master)
    }
    setTracks(list) {
      this.tracks = list || []
      if (this.index >= this.tracks.length) this.index = -1
    }
    title() {
      if (this.index < 0) return { title: '轻音乐', sub: '现场生成' }
      const name = this.tracks[this.index].replace(/\.[^.]+$/, '')
      return { title: name, sub: `第 ${this.index + 1} / ${this.tracks.length} 首` }
    }
    async play() {
      this.ensureCtx()
      if (this.ctx.state === 'suspended') await this.ctx.resume()
      if (this.index < 0 && this.tracks.length && !this.started) this.index = 0
      this.started = true
      this.playing = true
      if (this.index < 0) {
        this.audio.pause()
        this.song.start()
      } else {
        this.song.stop()
        const src = `/music/${encodeURIComponent(this.tracks[this.index])}`
        if (!this.audio.src.endsWith(src)) this.audio.src = src
        this.audio.volume = this.volume
        await this.audio.play().catch(() => {})
      }
      this.onchange()
    }
    pause() {
      this.playing = false
      this.song?.stop()
      this.audio.pause()
      this.onchange()
    }
    toggle() {
      return this.playing ? this.pause() : this.play()
    }
    next() {
      const total = this.tracks.length + 1
      const cur = this.index + 1 // 0 = 小曲
      this.index = ((cur + 1) % total) - 1
      if (this.playing) this.play()
      else this.onchange()
    }
    setVolume(v) {
      this.volume = v
      if (this.master) this.master.gain.value = v * 0.9
      this.audio.volume = v
      try { localStorage.setItem('yz-volume', String(v)) } catch {}
    }
    // 提示音：叮～
    ding() {
      this.ensureCtx()
      if (this.ctx.state === 'suspended') this.ctx.resume()
      const t = this.ctx.currentTime + 0.02
      ;[[88, 0], [93, 0.11]].forEach(([n, d]) => {
        const o = this.ctx.createOscillator()
        const g = this.ctx.createGain()
        o.type = 'sine'
        o.frequency.value = NOTE(n)
        o.connect(g); g.connect(this.ctx.destination)
        g.gain.setValueAtTime(0.0001, t + d)
        g.gain.exponentialRampToValueAtTime(0.18, t + d + 0.01)
        g.gain.exponentialRampToValueAtTime(0.0001, t + d + 0.5)
        o.start(t + d); o.stop(t + d + 0.55)
      })
    }
    // 敲门声
    knock() {
      this.ensureCtx()
      if (this.ctx.state === 'suspended') this.ctx.resume()
      const t = this.ctx.currentTime + 0.02
      ;[0, 0.17].forEach((d) => {
        const o = this.ctx.createOscillator()
        const g = this.ctx.createGain()
        const f = this.ctx.createBiquadFilter()
        o.type = 'triangle'
        o.frequency.setValueAtTime(190, t + d)
        o.frequency.exponentialRampToValueAtTime(70, t + d + 0.09)
        f.type = 'lowpass'
        f.frequency.value = 900
        o.connect(f); f.connect(g); g.connect(this.ctx.destination)
        g.gain.setValueAtTime(0.5, t + d)
        g.gain.exponentialRampToValueAtTime(0.0001, t + d + 0.14)
        o.start(t + d); o.stop(t + d + 0.16)
      })
    }
  }

  window.Radio = new Radio()
})()
