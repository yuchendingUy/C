// 柚柚本人（原创角色）：一颗圆滚滚的柚子，头顶一片叶子
// window.Mascot.render(el, { mood, act })
;(function () {
  const INK = 'var(--ink)'
  const W = 6

  const eyes = {
    normal: (dx = 0, dy = 0) => `
      <g class="blink">
        <ellipse cx="${84 + dx}" cy="${126 + dy}" rx="8.5" ry="11.5" fill="${INK}"/>
        <ellipse cx="${136 + dx}" cy="${126 + dy}" rx="8.5" ry="11.5" fill="${INK}"/>
        <circle cx="${87 + dx}" cy="${121 + dy}" r="3.4" fill="#fff"/>
        <circle cx="${139 + dx}" cy="${121 + dy}" r="3.4" fill="#fff"/>
      </g>`,
    big: () => `
      <g class="blink">
        <ellipse cx="84" cy="125" rx="10.5" ry="13.5" fill="${INK}"/>
        <ellipse cx="136" cy="125" rx="10.5" ry="13.5" fill="${INK}"/>
        <circle cx="88" cy="119" r="4.2" fill="#fff"/><circle cx="80.5" cy="130" r="2" fill="#fff"/>
        <circle cx="140" cy="119" r="4.2" fill="#fff"/><circle cx="132.5" cy="130" r="2" fill="#fff"/>
      </g>`,
    happy: () => `<path d="M74 129 Q84 116 94 129 M126 129 Q136 116 146 129" fill="none" stroke="${INK}" stroke-width="${W}" stroke-linecap="round"/>`,
    closed: () => `<path d="M75 127 Q84 135 93 127 M127 127 Q136 135 145 127" fill="none" stroke="${INK}" stroke-width="${W}" stroke-linecap="round"/>`,
    focus: () => `
      <path d="M74 127 H94 M126 127 H146" stroke="${INK}" stroke-width="${W + 1}" stroke-linecap="round"/>
      <path d="M73 113 L93 118 M147 113 L127 118" stroke="${INK}" stroke-width="4.5" stroke-linecap="round"/>`,
    pouty: () => `
      ${eyes.normal(4, 2)}
      <path d="M72 110 L94 117 M148 110 L126 117" stroke="${INK}" stroke-width="5" stroke-linecap="round"/>`,
    worried: () => `
      ${eyes.normal(0, 2)}
      <path d="M74 115 L93 109 M146 115 L127 109" stroke="${INK}" stroke-width="5" stroke-linecap="round"/>`,
    up: () => eyes.normal(-2, -5),
  }

  const mouths = {
    smile: `<path d="M99 147 Q104.5 155 110 147.5 Q115.5 155 121 147" fill="none" stroke="${INK}" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>`,
    open: `<path class="mouth-open" d="M100 145 Q110 165 120 145 Z" fill="#E8483A" stroke="${INK}" stroke-width="4.5" stroke-linejoin="round"/>`,
    talk: `<path class="mouth-talk" d="M101 146 Q110 162 119 146 Z" fill="#E8483A" stroke="${INK}" stroke-width="4.5" stroke-linejoin="round"/>`,
    o: `<ellipse cx="110" cy="151" rx="4.5" ry="5.5" fill="${INK}"/>`,
    flat: `<path d="M102 151 H118" stroke="${INK}" stroke-width="5" stroke-linecap="round"/>`,
    frown: `<path d="M100 155 Q110 145 120 155" fill="none" stroke="${INK}" stroke-width="5" stroke-linecap="round"/>`,
    pout: `<path d="M101 151 q4.5 -5 9 0 q4.5 5 9 0" fill="none" stroke="${INK}" stroke-width="5" stroke-linecap="round"/>`,
    cat: `<path d="M98 146 Q104 153 110 146 Q116 153 122 146" fill="none" stroke="${INK}" stroke-width="5" stroke-linecap="round"/>`,
  }

  const extras = {
    zzz: `<g class="zzz" font-family="ZCOOL KuaiLe, sans-serif" fill="${INK}" font-weight="700">
      <text x="160" y="70" font-size="26">Z</text><text x="180" y="48" font-size="20">z</text><text x="194" y="30" font-size="15">z</text></g>`,
    sparkle: `<g class="sparkles" fill="var(--yuzu)" stroke="${INK}" stroke-width="3" stroke-linejoin="round">
      <path d="M30 70 l5 12 12 5 -12 5 -5 12 -5 -12 -12 -5 12 -5z"/>
      <path d="M182 62 l4 9 9 4 -9 4 -4 9 -4 -9 -9 -4 9 -4z"/></g>`,
    heart: `<path class="float-heart" d="M178 74 c0 -10 14 -12 15 -2 c1 -10 15 -8 15 2 c0 10 -15 18 -15 18 s-15 -8 -15 -18z" fill="var(--pink)" stroke="${INK}" stroke-width="3.5"/>`,
    sweat: `<path class="sweat" d="M166 100 q8 12 0 18 q-8 -6 0 -18z" fill="#8FD3FF" stroke="${INK}" stroke-width="3"/>`,
    tear: `<path d="M78 138 q6 10 0 15 q-6 -5 0 -15z" fill="#8FD3FF" stroke="${INK}" stroke-width="3"/>`,
    dots: `<g class="think-dots" fill="#fff" stroke="${INK}" stroke-width="3">
      <circle cx="168" cy="82" r="5"/><circle cx="182" cy="64" r="7.5"/><circle cx="200" cy="42" r="10"/></g>`,
    laptop: `<g class="laptop">
      <path d="M62 170 H158 L150 136 H70 Z" fill="var(--cobalt)" stroke="${INK}" stroke-width="5" stroke-linejoin="round"/>
      <rect x="50" y="168" width="120" height="14" rx="5" fill="#C9D3FF" stroke="${INK}" stroke-width="5"/>
      <circle cx="110" cy="153" r="7" fill="var(--yuzu)" stroke="${INK}" stroke-width="3"/>
      <path d="M107 146 q3 -6 8 -6" stroke="var(--mint)" stroke-width="4" fill="none" stroke-linecap="round"/></g>`,
  }

  function face(mood, act) {
    if (act === 'sleeping') return { e: eyes.closed(), m: mouths.o, x: extras.zzz, blush: 1 }
    if (act === 'working') return { e: eyes.focus(), m: mouths.flat, x: extras.laptop, blush: 0.6 }
    if (act === 'thinking') return { e: eyes.up(), m: mouths.flat, x: extras.dots, blush: 1 }
    const talk = act === 'talking'
    const map = {
      happy: { e: eyes.normal(), m: mouths.smile },
      shy: { e: eyes.happy(), m: mouths.cat, x: extras.heart, blush: 1.5 },
      sleepy: { e: eyes.closed(), m: mouths.o, x: extras.zzz },
      pouty: { e: eyes.pouty(), m: mouths.pout, blush: 1.3 },
      excited: { e: eyes.big(), m: mouths.open, x: extras.sparkle, blush: 1.2 },
      worried: { e: eyes.worried(), m: mouths.frown, x: extras.sweat },
      focused: { e: eyes.focus(), m: mouths.flat },
      sad: { e: eyes.worried(), m: mouths.frown, x: extras.tear, blush: 0.6 },
    }
    const f = { blush: 1, x: '', ...(map[mood] || map.happy) }
    if (talk) f.m = mouths.talk
    return f
  }

  function svg(mood, act) {
    const f = face(mood, act)
    const b = f.blush
    return `<svg viewBox="0 0 220 220" class="mascot-svg" aria-hidden="true">
      <ellipse cx="110" cy="206" rx="64" ry="9" fill="var(--ink)" opacity=".18"/>
      <ellipse cx="86" cy="196" rx="16" ry="9" fill="var(--yuzu-deep)" stroke="${INK}" stroke-width="5"/>
      <ellipse cx="134" cy="196" rx="16" ry="9" fill="var(--yuzu-deep)" stroke="${INK}" stroke-width="5"/>
      <g class="arm-l"><ellipse cx="38" cy="140" rx="13" ry="17" fill="var(--yuzu)" stroke="${INK}" stroke-width="5"/></g>
      <g class="arm-r"><ellipse cx="182" cy="140" rx="13" ry="17" fill="var(--yuzu)" stroke="${INK}" stroke-width="5"/></g>
      <path d="M110 62 C 182 60, 196 112, 188 146 C 178 190, 140 198, 110 198 C 80 198, 42 190, 32 146 C 24 112, 38 60, 110 62 Z" fill="var(--yuzu)" stroke="${INK}" stroke-width="${W}"/>
      <ellipse cx="72" cy="94" rx="15" ry="8" transform="rotate(-32 72 94)" fill="#fff" opacity=".75"/>
      <g fill="var(--yuzu-deep)"><circle cx="150" cy="96" r="2.6"/><circle cx="160" cy="112" r="2.2"/><circle cx="58" cy="166" r="2.4"/><circle cx="160" cy="170" r="2.6"/><circle cx="140" cy="84" r="2"/><circle cx="66" cy="120" r="2"/></g>
      <path d="M110 64 q3 -12 -3 -22" fill="none" stroke="${INK}" stroke-width="${W}" stroke-linecap="round"/>
      <g class="leaf"><path d="M108 46 C 118 22, 152 18, 166 28 C 154 50, 126 56, 108 46 Z" fill="var(--mint)" stroke="${INK}" stroke-width="5" stroke-linejoin="round"/>
      <path d="M114 44 Q 136 36, 156 30" fill="none" stroke="${INK}" stroke-width="3" stroke-linecap="round"/></g>
      <g class="face">
        <ellipse cx="68" cy="148" rx="${11 * b}" ry="${6.5 * Math.min(b, 1.2)}" fill="var(--pink)" opacity=".9"/>
        <ellipse cx="152" cy="148" rx="${11 * b}" ry="${6.5 * Math.min(b, 1.2)}" fill="var(--pink)" opacity=".9"/>
        ${f.e}
        ${f.m}
      </g>
      ${f.x || ''}
    </svg>`
  }

  let last = ''
  window.Mascot = {
    render(el, { mood = 'happy', act = 'idle' } = {}) {
      const key = `${mood}|${act}`
      if (key === last && el.firstChild) return
      last = key
      el.innerHTML = svg(mood, act)
      el.dataset.act = act
      el.dataset.mood = mood
    },
    head() {
      return `<svg viewBox="22 14 176 196" aria-hidden="true">
        <path d="M110 62 C 182 60, 196 112, 188 146 C 178 190, 140 198, 110 198 C 80 198, 42 190, 32 146 C 24 112, 38 60, 110 62 Z" fill="var(--yuzu)" stroke="var(--ink)" stroke-width="9"/>
        <path d="M110 64 q3 -12 -3 -22" fill="none" stroke="var(--ink)" stroke-width="8" stroke-linecap="round"/>
        <path d="M108 46 C 118 22, 152 18, 166 28 C 154 50, 126 56, 108 46 Z" fill="var(--mint)" stroke="var(--ink)" stroke-width="7" stroke-linejoin="round"/>
        <ellipse cx="68" cy="150" rx="12" ry="7" fill="var(--pink)"/><ellipse cx="152" cy="150" rx="12" ry="7" fill="var(--pink)"/>
        <ellipse cx="84" cy="126" rx="10" ry="13" fill="var(--ink)"/><ellipse cx="136" cy="126" rx="10" ry="13" fill="var(--ink)"/>
        <circle cx="88" cy="120" r="4" fill="#fff"/><circle cx="140" cy="120" r="4" fill="#fff"/>
        <path d="M98 148 Q104 156 110 148 Q116 156 122 148" fill="none" stroke="var(--ink)" stroke-width="6" stroke-linecap="round"/>
      </svg>`
    },
  }
})()
