;(() => {
  const $ = (s, el = document) => el.querySelector(s)
  const $$ = (s, el = document) => [...el.querySelectorAll(s)]

  const S = {
    settings: null, state: {}, features: { tools: {}, moods: {} }, convs: [], mainId: null,
    conv: null, running: new Set(), drafts: new Map(), runningTools: new Set(), toolLabel: '',
    showOlder: false, attachments: [], compacting: false, loaded: false, needLogin: true,
  }
  const ui = {
    history: $('#history'), live: $('#live'), scroller: $('#scroller'), input: $('#input'), send: $('#sendBtn'),
    mascot: $('#mascot'), say: $('#say'), sheet: $('#sheet'), toasts: $('#toasts'), jump: $('#jumpBtn'),
  }

  // ---------------- 小工具 ----------------
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c])
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
  const store = {
    get(k) { try { return localStorage.getItem(k) } catch { return null } },
    set(k, v) { try { localStorage.setItem(k, v) } catch {} },
  }
  const coarse = matchMedia('(pointer: coarse)').matches

  async function api(path, opts = {}) {
    const init = { method: opts.method || 'GET', headers: {}, credentials: 'same-origin' }
    if (opts.body !== undefined) {
      if (opts.raw) init.body = opts.body
      else { init.body = JSON.stringify(opts.body); init.headers['Content-Type'] = 'application/json' }
    }
    Object.assign(init.headers, opts.headers || {})
    const res = await fetch(path, init)
    let data = null
    try { data = await res.json() } catch {}
    if (res.status === 401 && !path.startsWith('/api/login')) {
      showDoor(true)
      throw new Error('先对暗号')
    }
    if (!res.ok) throw Object.assign(new Error(data?.error || `出错了（${res.status}）`), { status: res.status })
    return data
  }

  function toast(text, kind = '', ms = 3200, onclick) {
    const el = document.createElement('div')
    el.className = `toast ${kind}`
    el.textContent = text
    if (onclick) el.addEventListener('click', () => { onclick(); el.remove() })
    ui.toasts.append(el)
    setTimeout(() => el.remove(), ms)
  }

  const tz = () => S.settings?.timezone || 'Asia/Shanghai'
  function parts(ts) {
    const f = new Intl.DateTimeFormat('zh-CN', { timeZone: tz(), year: 'numeric', month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hour12: false })
    const p = Object.fromEntries(f.formatToParts(new Date(ts)).map((x) => [x.type, x.value]))
    return { y: +p.year, mo: +p.month, d: +p.day, h: +p.hour % 24, mi: p.minute }
  }
  function fmtTag(ts) {
    const p = parts(ts)
    const n = parts(Date.now())
    const y = parts(Date.now() - 864e5)
    const clock = `${String(p.h).padStart(2, '0')}:${p.mi}`
    if (p.y === n.y && p.mo === n.mo && p.d === n.d) return `今天 ${clock}`
    if (p.y === y.y && p.mo === y.mo && p.d === y.d) return `昨天 ${clock}`
    return `${p.y !== n.y ? `${p.y}年` : ''}${p.mo}月${p.d}日 ${clock}`
  }
  const fmtShort = (ts) => {
    const t = fmtTag(ts)
    return t.startsWith('今天') ? t.slice(3) : t.split(' ')[0]
  }

  // ---------------- 文字渲染 ----------------
  function inline(text) {
    let s = esc(text)
    s = s.replace(/`([^`\n]+)`/g, '<code>$1</code>')
    s = s.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>')
    s = s.replace(/\[([^\]\n]+)\]\((https?:\/\/[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>')
    s = s.replace(/(^|[\s(（])(https?:\/\/[^\s<)）]+)/g, '$1<a href="$2" target="_blank" rel="noopener">$2</a>')
    return s.replace(/\n/g, '<br>')
  }
  // 一条消息按空行拆成多个气泡，代码块单独一个
  function bubbles(text) {
    const out = []
    const re = /```(\w*)[^\n]*\n?([\s\S]*?)(```|$)/g
    let last = 0
    let m
    const pushText = (t) => {
      for (const para of t.split(/\n\s*\n/)) if (para.trim()) out.push({ html: inline(para.trim()) })
    }
    while ((m = re.exec(text))) {
      pushText(text.slice(last, m.index))
      out.push({ code: true, html: `${m[1] ? `<span class="lang">${esc(m[1])}</span>` : ''}<pre><code>${esc(m[2].replace(/\n$/, ''))}</code></pre>` })
      last = re.lastIndex
      if (!m[3]) break
    }
    pushText(text.slice(last))
    return out
  }

  // ---------------- 图标 ----------------
  const P = {
    bash: 'M4 5h16v14H4zM8 10l3 2-3 2M13 15h4',
    read: 'M6 3h9l4 4v14H6zM14 3v5h5M9 13h7M9 17h5',
    write: 'M6 3h9l4 4v14H6zM9 17l1-3 5-5 2 2-5 5z',
    edit: 'M4 20l4-1 11-11-3-3L5 16zM14 7l3 3',
    find: 'M10.5 17a6.5 6.5 0 1 1 0-13 6.5 6.5 0 0 1 0 13zM15.5 15.5L20 20',
    pulse: 'M3 12h4l2-5 4 10 2-5h6',
    globe: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM3 12h18M12 3c3 3.5 3 14.5 0 18M12 3c-3 3.5-3 14.5 0 18',
    click: 'M6 3l12 7-5 1.5L10.5 17zM13 12l5 6',
    keys: 'M3 7h18v10H3zM7 11h.01M11 11h.01M15 11h.01M8 14h8',
    note: 'M5 3h11a3 3 0 0 1 3 3v15H8a3 3 0 0 1-3-3zM9 8h6M9 12h6',
    clock: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 7v5l3 2',
    smile: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM8.5 14c2 2.2 5 2.2 7 0M9 9.5h.01M15 9.5h.01',
    x: 'M6 6l12 12M18 6L6 18',
    mail: 'M3 6h18v12H3zM3 6l9 7 9-7',
    trash: 'M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13',
    pen: 'M4 20l4-1 11-11-3-3L5 16z',
  }
  const svg = (d) => `<svg viewBox="0 0 24 24"><path d="${d}"/></svg>`
  const TOOL_ICON = {
    bash: 'bash', read: 'read', write: 'write', edit: 'edit', glob: 'find', grep: 'find', sysinfo: 'pulse',
    webfetch: 'globe', browser_open: 'globe', browser_look: 'globe', browser_scroll: 'globe', browser_back: 'globe', browser_close: 'globe',
    browser_click: 'click', browser_type: 'keys',
    remember: 'note', forget: 'note', schedule_message: 'clock', list_schedules: 'clock', cancel_schedule: 'clock', set_mood: 'smile',
  }
  const LIFE = new Set(['remember', 'forget', 'schedule_message', 'list_schedules', 'cancel_schedule', 'set_mood'])

  // ---------------- 房间 ----------------
  const LEVELS = [[85, '离不开你'], [60, '特别的人'], [35, '好朋友'], [15, '熟起来了'], [0, '刚认识']]
  const IDLE_SAY = {
    happy: ['在晃腿', '哼着小曲', '在数内存条'], shy: ['脸红红的', '偷偷看你'], sleepy: ['打了个哈欠', '眼皮在打架'],
    pouty: ['鼓着脸', '哼'], excited: ['蹦蹦跳跳', '超有精神'], worried: ['有点担心', '皱着眉'],
    focused: ['在认真', '盯着屏幕'], sad: ['蔫蔫的', '抱着膝盖'],
  }
  const PET_SAY = ['干嘛啦～', '嘿嘿', '叶子要被你摸掉啦', '再摸要收费了哦', '(＞ω＜)', '痒痒的', '哼，才没有很开心', '再来一下也可以']

  function timeOfDay() {
    const h = parts(Date.now()).h
    if (h >= 5 && h < 8) return 'dawn'
    if (h >= 8 && h < 17) return 'day'
    if (h >= 17 && h < 19) return 'dusk'
    return 'night'
  }

  let sayLock = 0
  function updateRoom() {
    const st = S.state || {}
    const mood = st.mood || 'happy'
    let act = 'idle'
    const running = S.conv && S.running.has(S.conv.id)
    const drafting = [...S.drafts.values()].some((d) => d.content)
    if (S.running.size) act = S.runningTools.size ? 'working' : drafting ? 'talking' : 'thinking'
    else if (st.asleep) act = 'sleeping'
    Mascot.render(ui.mascot, { mood, act })
    $('#window').dataset.time = timeOfDay()
    $('#moodChip').textContent = S.features.moods?.[mood] || '开心'
    if (Date.now() > sayLock) {
      const say = act === 'working' ? `在${S.toolLabel || '干活'}` : act === 'talking' ? '在打字' : act === 'thinking' ? '在想怎么回你' : act === 'sleeping' ? '睡着了' : null
      if (say) ui.say.textContent = say
      else if (!ui.say.dataset.idle || ui.say.dataset.mood !== mood) {
        const pool = IDLE_SAY[mood] || IDLE_SAY.happy
        ui.say.textContent = pool[Math.floor(Math.random() * pool.length)]
        ui.say.dataset.mood = mood
      }
      ui.say.dataset.idle = say ? '' : '1'
    }
    const aff = Math.max(0, Math.min(100, st.affection ?? 0))
    $('#meterNum').textContent = aff
    $('#meterLevel').textContent = LEVELS.find(([n]) => aff >= n)[1]
    $('#meterFill').style.width = `${Math.max(4, aff)}%`
    $('#mmFill').style.width = `${Math.max(4, aff)}%`
    void running
  }

  function applyNames() {
    const name = S.settings?.charName || '柚柚'
    $('#charName').textContent = name
    $('#plateName').textContent = name
    document.title = `${name}的房间`
    ui.input.placeholder = `跟${name}说点什么……`
  }

  ui.mascot.addEventListener('click', async () => {
    ui.mascot.classList.remove('boing')
    void ui.mascot.offsetWidth
    ui.mascot.classList.add('boing')
    setTimeout(() => ui.mascot.classList.remove('boing'), 650)
    ui.say.textContent = PET_SAY[Math.floor(Math.random() * PET_SAY.length)]
    sayLock = Date.now() + 2500
    try {
      const r = await api('/api/pet', { method: 'POST', body: {} })
      S.state = { ...S.state, ...r.state }
      updateRoom()
    } catch {}
  })

  // ---------------- 收音机 ----------------
  const radio = window.Radio
  radio.onchange = () => {
    $('#radio').classList.toggle('playing', radio.playing)
    const t = radio.title()
    $('#radioTitle').textContent = t.title
    $('#radioSub').textContent = t.sub
  }
  $('#radioPlay').addEventListener('click', () => {
    radio.toggle()
    store.set('yz-music', radio.playing ? 'on' : 'off')
  })
  $('#radioNext').addEventListener('click', () => radio.next())
  async function loadMusic() {
    try {
      radio.setTracks(await api('/api/music'))
      radio.onchange()
    } catch {}
  }

  // ---------------- 门口 ----------------
  const door = $('#door')
  function showDoor(needLogin) {
    S.needLogin = needLogin
    door.classList.remove('gone', 'open', 'knocking')
    $('#app').hidden = true
    $('#doorPass').hidden = !needLogin
    $('#doorLabel').textContent = needLogin ? '对个暗号' : `${S.settings?.charName || '柚柚'}在里面等你`
    $('#knockBtn').textContent = needLogin ? '咚咚，敲门' : '咚咚，进去啦'
    const unread = S.convs.reduce((n, c) => n + (c.unread || 0), 0)
    const note = $('#doorNote')
    note.hidden = needLogin || !unread
    note.textContent = unread ? `门缝里塞着 ${unread} 张小纸条` : ''
    if (needLogin) setTimeout(() => $('#doorPass').focus(), 50)
  }

  $('#doorForm').addEventListener('submit', async (e) => {
    e.preventDefault()
    radio.knock() // 同时解锁浏览器的声音
    door.classList.remove('knocking')
    void door.offsetWidth
    door.classList.add('knocking')
    $('#doorErr').textContent = ''
    try {
      if (S.needLogin) {
        await api('/api/login', { method: 'POST', body: { password: $('#doorPass').value } })
        $('#doorPass').value = ''
      }
      if (!S.loaded) await loadAll()
    } catch (err) {
      $('#doorErr').textContent = err.message
      return
    }
    await sleep(500)
    door.classList.add('open')
    await sleep(900)
    door.classList.add('gone')
    $('#app').hidden = false
    scrollBottom(true)
    if (store.get('yz-music') !== 'off') radio.play()
    if (!coarse) ui.input.focus()
  })

  // ---------------- 数据 ----------------
  async function loadAll() {
    const st = await api('/api/state')
    S.settings = st.settings
    S.state = st.state
    S.features = st.features
    S.convs = st.conversations
    S.mainId = st.mainId
    S.running = new Set(st.running)
    S.loaded = true
    applyNames()
    await loadConv(st.mainId)
    loadMusic()
    connectEvents()
    updateRoom()
    updateBadges()
  }

  async function refreshState() {
    try {
      const st = await api('/api/state')
      S.state = st.state
      S.convs = st.conversations
      S.running = new Set(st.running)
      updateRoom()
      updateBadges()
    } catch {}
  }

  async function loadConv(id) {
    const c = await api(`/api/conversations/${encodeURIComponent(id)}`)
    S.conv = c
    S.showOlder = false
    S.drafts.clear()
    $('#convTitle').textContent = c.title
    updateSub()
    renderHistory()
    renderLive()
    scrollBottom(true)
    markRead()
  }

  function updateSub() {
    const c = S.conv
    if (!c) return
    const n = c.messages.filter((m) => !m.hidden && (m.role === 'user' || (m.role === 'assistant' && m.content))).length
    $('#convSub').textContent = `${fmtShort(c.createdAt)} 翻开的，聊了 ${n} 句${c.summary ? '，有一页日记' : ''}`
  }

  async function markRead() {
    const c = S.conv
    if (!c || document.hidden) return
    const item = S.convs.find((x) => x.id === c.id)
    if ((c.unread || item?.unread) && !$('#app').hidden) {
      c.unread = 0
      if (item) item.unread = 0
      updateBadges()
      api(`/api/conversations/${c.id}/read`, { method: 'POST', body: {} }).catch(() => {})
    }
  }

  function updateBadges() {
    const others = S.convs.filter((c) => c.id !== S.conv?.id).reduce((n, c) => n + (c.unread || 0), 0)
    const b = $('#diaryBadge')
    b.hidden = !others
    b.textContent = others
  }

  // ---------------- 渲染聊天 ----------------
  const nearBottom = () => ui.scroller.scrollHeight - ui.scroller.scrollTop - ui.scroller.clientHeight < 160
  function scrollBottom(force) {
    if (force || nearBottom()) {
      requestAnimationFrame(() => { ui.scroller.scrollTop = ui.scroller.scrollHeight })
      ui.jump.hidden = true
    } else ui.jump.hidden = false
  }
  ui.scroller.addEventListener('scroll', () => { if (nearBottom()) ui.jump.hidden = true })
  ui.jump.addEventListener('click', () => scrollBottom(true))

  const parseArgs = (s) => { try { return JSON.parse(s || '{}') } catch { return {} } }
  function toolSummary(name, a) {
    switch (name) {
      case 'bash': return a.description ? `${a.description}：${a.command}` : a.command
      case 'read': case 'write': case 'edit': return a.path
      case 'glob': case 'grep': return `${a.pattern}${a.path ? `  在 ${a.path}` : ''}`
      case 'webfetch': case 'browser_open': return a.url
      case 'browser_click': return `元素 [${a.ref}]`
      case 'browser_type': return `「${a.text}」`
      case 'browser_scroll': return { down: '往下', up: '往上', top: '到顶', bottom: '到底' }[a.direction] || ''
      case 'remember': return a.text
      case 'forget': case 'cancel_schedule': return a.id
      case 'schedule_message': return `${a.at || `${a.in_minutes} 分钟后`}  ${a.note}${a.repeat === 'daily' ? '（每天）' : ''}`
      case 'set_mood': return S.features.moods?.[a.mood] || a.mood
      default: return ''
    }
  }
  function toolState(call, result) {
    if (!result) {
      if (S.runningTools.has(call.id) || (S.conv && S.running.has(S.conv.id))) return ['run', '在做']
      return ['bad', '没做完']
    }
    const t = result.content || ''
    if (/^(错误|出错了)/.test(t)) return ['bad', '出错了']
    const code = /\[退出码 (\d+)\]\s*$/.exec(t)
    if (code && code[1] !== '0') return ['bad', `退出码 ${code[1]}`]
    if (/\[超过 \d+ 秒被停止了/.test(t)) return ['bad', '超时了']
    return ['ok', '好了']
  }
  const clipText = (t, n) => (t.length > n ? `${t.slice(0, n)}\n……（还有 ${t.length - n} 字）` : t)

  function actCard(call, result) {
    const name = call.name
    const meta = S.features.tools?.[name] || { label: name }
    const a = parseArgs(call.arguments)
    const [cls, label] = toolState(call, result)
    const kind = LIFE.has(name) ? 'life' : /^(webfetch|browser_)/.test(name) ? 'web' : ''
    let body = ''
    if (name === 'bash') body += `<span class="label">命令</span><pre>${esc(a.command || '')}</pre>`
    else if (name === 'write') body += `<span class="label">写进 ${esc(a.path)}</span><pre>${esc(clipText(String(a.content || ''), 4000))}</pre>`
    else if (name === 'edit') body += `<span class="label">改 ${esc(a.path)}</span><pre>${esc(`- ${clipText(String(a.oldString || ''), 2000)}\n+ ${clipText(String(a.newString || ''), 2000)}`)}</pre>`
    else if (!LIFE.has(name) && Object.keys(a).length) body += `<span class="label">参数</span><pre>${esc(JSON.stringify(a, null, 2))}</pre>`
    if (result) {
      body += `<span class="label">结果</span><pre>${esc(clipText(result.content || '', 12000))}</pre>`
      if (result.images?.length) body += `<div class="imgs">${result.images.map((id) => `<img src="/api/files/${encodeURIComponent(id)}" alt="截图" loading="lazy">`).join('')}</div>`
    }
    const shot = result?.images?.length ? `<div class="imgs" style="padding:0 12px 10px">${result.images.map((id) => `<img src="/api/files/${encodeURIComponent(id)}" alt="截图" loading="lazy">`).join('')}</div>` : ''
    return `<details class="act ${kind}">
      <summary><span class="act-icon">${svg(P[TOOL_ICON[name]] || P.bash)}</span>
      <span class="act-text"><b>${esc(meta.label || name)}</b><code>${esc(toolSummary(name, a))}</code></span>
      <span class="act-state ${cls}">${label}</span></summary>
      ${shot}
      <div class="act-body">${body}</div></details>`
  }

  const TRIGGER_TAG = { idle: '跑来找你了', wake: '刚睡醒就来找你', reminder: '按约好的时间来了' }

  function renderHistory() {
    const c = S.conv
    if (!c) return
    const results = new Map(c.messages.filter((m) => m.role === 'tool').map((m) => [m.tool_call_id, m]))
    const archived = c.messages.filter((m) => m.archived && !m.hidden && (m.role === 'user' || (m.role === 'assistant' && m.content))).length
    const list = c.messages.filter((m) => S.showOlder || !m.archived)
    const head = Mascot.head()
    const name = esc(S.settings?.charName || '柚柚')
    let html = ''
    if (archived && !S.showOlder) {
      html += `<div class="strip info older">更早的 ${archived} 条已经收进日记了
        <button class="btn" data-act="older" type="button">翻出来看</button>
        <button class="btn" data-act="diary" type="button">读日记</button></div>`
    }
    let group = null
    let lastTs = 0
    let trigger = null
    const flush = () => {
      if (!group) return
      if (group.side === 'me') {
        html += `<div class="row me"><div class="stack">${group.items.map((m) => {
          let h = ''
          if (m.images?.length) h += `<div class="imgs">${m.images.map((id) => `<img src="/api/files/${encodeURIComponent(id)}" alt="图片">`).join('')}</div>`
          h += bubbles(m.content || '').map((b) => `<div class="bubble${b.code ? ' codeblock' : ''}">${b.html}</div>`).join('')
          return h
        }).join('')}</div></div>`
      } else {
        let first = true
        let inner = group.trigger ? `<span class="proactive-tag">${svg(P.mail)}${name}${TRIGGER_TAG[group.trigger] || '来找你了'}</span>` : ''
        for (const m of group.items) {
          if (m.reasoning_content) inner += `<details class="thought"><summary>偷看${name}的心里话</summary><div class="thought-body">${esc(m.reasoning_content)}</div></details>`
          for (const b of bubbles(m.content || '')) {
            inner += `<div class="bubble${b.code ? ' codeblock' : ''}${first && !b.code ? ' taped' : ''}">${b.html}</div>`
            first = false
          }
          for (const call of m.tool_calls || []) inner += actCard(call, results.get(call.id))
        }
        if (inner) html += `<div class="row her"><div class="avatar">${head}</div><div class="stack">${inner}</div></div>`
      }
      group = null
    }
    for (const m of list) {
      if (m.role === 'tool') continue
      if (m.role === 'user' && m.hidden) { trigger = m.trigger || 'idle'; continue }
      if (m.role === 'event') {
        flush()
        if (m.kind === 'compact') html += `<div class="strip">${esc(m.content)} <button class="btn" data-act="diary" type="button">读日记</button></div>`
        else if (m.kind === 'error') html += `<div class="strip error"><b>出了点状况</b> ${esc(m.content)}</div>`
        else html += `<div class="strip info">${esc(m.content)}</div>`
        continue
      }
      const side = m.role === 'user' ? 'me' : 'her'
      if (m.ts - lastTs > 10 * 60 * 1000) {
        flush()
        html += `<div class="time-tag">${fmtTag(m.ts)}</div>`
      }
      lastTs = m.ts
      if (!group || group.side !== side || (side === 'her' && trigger)) {
        flush()
        group = { side, items: [], trigger: side === 'her' ? trigger : null }
        if (side === 'her') trigger = null
      }
      if (side === 'me') trigger = null
      group.items.push(m)
    }
    flush()
    if (!list.some((m) => !m.hidden && m.role !== 'event')) {
      html += `<div class="strip info">这一页还是空白的，跟${name}打个招呼吧</div>`
    }
    if (!S.features.apiKey) {
      html += `<div class="strip error"><b>还差一把钥匙</b> 填上 DeepSeek 密钥，${name}才能说话 <button class="btn" data-act="key" type="button">去填</button></div>`
    }
    ui.history.innerHTML = html
  }

  let liveQueued = false
  function renderLive() {
    if (liveQueued) return
    liveQueued = true
    requestAnimationFrame(() => {
      liveQueued = false
      const atBottom = nearBottom()
      const c = S.conv
      let html = ''
      if (c && S.running.has(c.id)) {
        const head = Mascot.head()
        const name = esc(S.settings?.charName || '柚柚')
        const drafts = [...S.drafts.values()].filter((d) => d.convId === c.id)
        for (const d of drafts) {
          let inner = ''
          if (d.reasoning) inner += `<details class="thought"${d.content ? '' : ' open'}><summary>${name}在想……</summary><div class="thought-body">${esc(d.reasoning.slice(-3000))}</div></details>`
          const bs = bubbles(d.content)
          bs.forEach((b, i) => { inner += `<div class="bubble${b.code ? ' codeblock' : ''}${i === bs.length - 1 && !b.code ? ' cursor' : ''}">${b.html}</div>` })
          if (!d.content && !d.reasoning) inner += `<div class="bubble"><span class="typing"><i></i><i></i><i></i></span></div>`
          html += `<div class="row her"><div class="avatar">${head}</div><div class="stack">${inner}</div></div>`
        }
        if (!drafts.length && !S.runningTools.size) {
          html += `<div class="row her"><div class="avatar">${head}</div><div class="stack"><div class="bubble"><span class="typing"><i></i><i></i><i></i></span></div></div></div>`
        }
      }
      if (S.compacting) html += `<div class="strip info">正在把前面的聊天折成一页日记……</div>`
      ui.live.innerHTML = html
      // 保持思考框滚在最新处
      $$('.live .thought-body').forEach((el) => { el.scrollTop = el.scrollHeight })
      if (atBottom) ui.scroller.scrollTop = ui.scroller.scrollHeight
      else if (html) ui.jump.hidden = false
      updateComposer()
      updateRoom()
    })
  }

  function updateComposer() {
    const busy = S.conv && S.running.has(S.conv.id)
    ui.send.classList.toggle('stop', !!busy)
    ui.send.textContent = busy ? '停一下' : '发出去'
  }

  ui.history.addEventListener('click', (e) => {
    const b = e.target.closest('[data-act]')
    if (b) {
      const act = b.dataset.act
      if (act === 'older') { S.showOlder = true; renderHistory() }
      if (act === 'diary') openSheet('memory', 'diary')
      if (act === 'key') openSheet('settings', 'keys')
    }
  })
  document.addEventListener('click', (e) => {
    const img = e.target.closest('.imgs img, img.shot')
    if (!img) return
    const v = document.createElement('div')
    v.className = 'viewer'
    v.innerHTML = `<img src="${img.src}" alt="">`
    v.addEventListener('click', () => v.remove())
    document.body.append(v)
  })

  // ---------------- 实时事件 ----------------
  let es = null
  let titleFlash = null
  function flashTitle(text) {
    if (titleFlash || !document.hidden) return
    const base = document.title
    let on = false
    titleFlash = setInterval(() => { document.title = (on = !on) ? text : base }, 1000)
    const stop = () => {
      if (document.hidden) return
      clearInterval(titleFlash)
      titleFlash = null
      document.title = base
      document.removeEventListener('visibilitychange', stop)
    }
    document.addEventListener('visibilitychange', stop)
  }

  function connectEvents() {
    if (es) es.close()
    es = new EventSource('/api/events')
    let opened = false
    es.onopen = () => {
      if (opened) { refreshState(); if (S.conv) reloadConv() }
      opened = true
    }
    es.onmessage = (e) => {
      let ev
      try { ev = JSON.parse(e.data) } catch { return }
      handleEvent(ev)
    }
  }

  async function reloadConv() {
    if (!S.conv) return
    try {
      const c = await api(`/api/conversations/${S.conv.id}`)
      if (c.id !== S.conv.id) return
      S.conv = c
      $('#convTitle').textContent = c.title
      updateSub()
      renderHistory()
      renderLive()
      scrollBottom()
    } catch {}
  }

  let convsTimer = null
  const refreshConvs = () => {
    clearTimeout(convsTimer)
    convsTimer = setTimeout(async () => {
      try { S.convs = await api('/api/conversations'); updateBadges() } catch {}
    }, 400)
  }

  function addToConv(msg) {
    if (!S.conv || S.conv.messages.some((m) => m.id === msg.id)) return false
    S.conv.messages.push(msg)
    return true
  }

  function handleEvent(ev) {
    const here = S.conv && ev.convId === S.conv.id
    switch (ev.type) {
      case 'user_message':
        if (here && addToConv(ev.message)) { renderHistory(); scrollBottom(true) }
        refreshConvs()
        break
      case 'turn_start':
        S.running.add(ev.convId)
        renderLive()
        break
      case 'assistant_start':
        S.drafts.set(ev.id, { convId: ev.convId, content: '', reasoning: '' })
        renderLive()
        break
      case 'reasoning': {
        const d = S.drafts.get(ev.id)
        if (d) { d.reasoning += ev.text; renderLive() }
        break
      }
      case 'content': {
        const d = S.drafts.get(ev.id)
        if (d) { d.content += ev.text; renderLive() }
        break
      }
      case 'assistant_done':
        S.drafts.delete(ev.message.id)
        if (here && addToConv(ev.message)) { renderHistory(); updateSub() }
        renderLive()
        break
      case 'tool_start':
        S.runningTools.add(ev.callId)
        S.toolLabel = ev.label || ''
        if (here) renderHistory()
        renderLive()
        break
      case 'tool_done':
        S.runningTools.delete(ev.callId)
        if (here) { addToConv(ev.message); renderHistory() }
        renderLive()
        break
      case 'event':
        if (here && addToConv(ev.message)) renderHistory()
        break
      case 'turn_end':
        S.running.delete(ev.convId)
        for (const [id, d] of S.drafts) if (d.convId === ev.convId) S.drafts.delete(id)
        S.runningTools.clear()
        if (here) reloadConv()
        renderLive()
        refreshConvs()
        break
      case 'proactive': {
        refreshConvs()
        const name = S.settings?.charName || '柚柚'
        if (store.get('yz-ding') !== 'off') radio.ding()
        if (here && !document.hidden && !$('#app').hidden) markRead()
        else if (!here) toast(`${name}在另一页找你`, 'love', 6000, () => switchConv(ev.convId))
        flashTitle(`(新消息) ${name}找你啦`)
        if (document.hidden && 'Notification' in window && Notification.permission === 'granted' && !navigator.serviceWorker?.controller) {
          try { new Notification(name, { body: '有新消息', icon: '/icons/icon.svg', tag: 'yuzu' }) } catch {}
        }
        break
      }
      case 'compacting':
        if (here) { S.compacting = true; renderLive() }
        break
      case 'compacted':
      case 'compact_failed':
        if (here) { S.compacting = false; reloadConv(); if (ev.type === 'compacted') toast('前面的聊天折成了一页日记') }
        break
      case 'mood':
        S.state.mood = ev.mood
        updateRoom()
        break
      case 'stats':
        S.state = { ...S.state, ...ev.state }
        updateRoom()
        break
      case 'settings':
        S.settings = ev.settings
        S.features = ev.features
        applyNames()
        updateRoom()
        break
    }
  }

  document.addEventListener('visibilitychange', () => {
    if (!document.hidden) { markRead(); updateRoom() }
  })
  setInterval(updateRoom, 60 * 1000)
  setInterval(refreshState, 5 * 60 * 1000)

  // ---------------- 发消息 ----------------
  function autosize() {
    ui.input.style.height = 'auto'
    ui.input.style.height = `${Math.min(ui.input.scrollHeight + 6, window.innerHeight * 0.4)}px`
  }
  ui.input.addEventListener('input', autosize)
  ui.input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing && e.keyCode !== 229 && !coarse) {
      e.preventDefault()
      $('#composer').requestSubmit()
    }
  })

  async function shrinkImage(file) {
    const url = URL.createObjectURL(file)
    try {
      const img = await new Promise((res, rej) => { const i = new Image(); i.onload = () => res(i); i.onerror = rej; i.src = url })
      const max = 1600
      const k = Math.min(1, max / Math.max(img.width, img.height))
      const c = document.createElement('canvas')
      c.width = Math.round(img.width * k)
      c.height = Math.round(img.height * k)
      const g = c.getContext('2d')
      g.fillStyle = '#fff'
      g.fillRect(0, 0, c.width, c.height)
      g.drawImage(img, 0, 0, c.width, c.height)
      return c.toDataURL('image/jpeg', 0.85)
    } finally {
      URL.revokeObjectURL(url)
    }
  }
  async function addFiles(files) {
    for (const f of files) {
      if (!f.type.startsWith('image/')) continue
      if (S.attachments.length >= 6) { toast('一次最多 6 张图'); break }
      try { S.attachments.push(await shrinkImage(f)) } catch { toast('这张图读不出来', 'bad') }
    }
    renderAttachments()
  }
  function renderAttachments() {
    $('#attachments').innerHTML = S.attachments.map((d, i) => `<div class="att"><img src="${d}" alt=""><button type="button" data-i="${i}" aria-label="去掉">×</button></div>`).join('')
  }
  $('#attachments').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-i]')
    if (b) { S.attachments.splice(+b.dataset.i, 1); renderAttachments() }
  })
  $('#fileInput').addEventListener('change', (e) => { addFiles([...e.target.files]); e.target.value = '' })
  ui.input.addEventListener('paste', (e) => {
    const files = [...(e.clipboardData?.files || [])].filter((f) => f.type.startsWith('image/'))
    if (files.length) { e.preventDefault(); addFiles(files) }
  })

  $('#composer').addEventListener('submit', async (e) => {
    e.preventDefault()
    if (!S.conv) return
    if (S.running.has(S.conv.id)) {
      api('/api/chat/stop', { method: 'POST', body: { convId: S.conv.id } }).catch(() => {})
      return
    }
    const text = ui.input.value.trim()
    const images = S.attachments.slice()
    if (!text && !images.length) return
    ui.input.value = ''
    S.attachments = []
    renderAttachments()
    autosize()
    S.running.add(S.conv.id)
    renderLive()
    try {
      const r = await api('/api/chat', { method: 'POST', body: { convId: S.conv.id, text, images } })
      if (addToConv(r.message)) { renderHistory(); scrollBottom(true) }
    } catch (err) {
      S.running.delete(S.conv.id)
      renderLive()
      ui.input.value = text
      S.attachments = images
      renderAttachments()
      autosize()
      toast(err.message, 'bad', 5000)
    }
  })

  // ---------------- 顶栏按钮 ----------------
  $('#newPageBtn').addEventListener('click', newPage)
  async function newPage() {
    try {
      const c = await api('/api/conversations', { method: 'POST', body: {} })
      S.convs.unshift({ id: c.id, title: c.title, createdAt: c.createdAt, updatedAt: c.updatedAt, unread: 0, preview: '' })
      await loadConv(c.id)
      ui.sheet.close()
      toast('翻开了新的一页')
    } catch (err) { toast(err.message, 'bad') }
  }
  async function switchConv(id) {
    await loadConv(id)
    updateBadges()
  }
  $('#compactBtn').addEventListener('click', () => compactNow())
  async function compactNow() {
    if (!S.conv) return
    if (!confirm('把前面的聊天折成一页日记？最近的几句会原样留着，她也不会忘记重要的事。')) return
    S.compacting = true
    renderLive()
    try {
      const r = await api(`/api/conversations/${S.conv.id}/compact`, { method: 'POST', body: {} })
      if (!r.ok) toast(r.reason)
    } catch (err) { toast(err.message, 'bad') }
    S.compacting = false
    renderLive()
  }

  // ---------------- 弹窗 ----------------
  $$('[data-open]').forEach((b) => b.addEventListener('click', () => openSheet(b.dataset.open)))
  ui.sheet.addEventListener('click', (e) => {
    if (e.target === ui.sheet) ui.sheet.close()
    if (e.target.closest('[data-close]')) ui.sheet.close()
  })

  const closeBtn = `<button class="icon-btn" type="button" data-close aria-label="关上">${svg(P.x)}</button>`

  function openSheet(kind, tab) {
    if (kind === 'diary') renderDiary()
    if (kind === 'memory') renderMemory(tab)
    if (kind === 'settings') renderSettings(tab)
    if (!ui.sheet.open) ui.sheet.showModal()
  }

  function renderDiary() {
    const items = S.convs.map((c) => `
      <div class="list-item clickable${c.id === S.conv?.id ? ' current' : ''}" data-id="${c.id}">
        <div class="grow"><b>${esc(c.title)}</b><span>${fmtShort(c.updatedAt)}　${esc(c.preview || '还是空白的')}</span></div>
        ${c.unread ? `<em class="badge" style="position:static">${c.unread}</em>` : ''}
        <button class="icon-btn x" type="button" data-rename="${c.id}" aria-label="改名">${svg(P.pen)}</button>
        <button class="icon-btn x" type="button" data-del="${c.id}" aria-label="撕掉">${svg(P.trash)}</button>
      </div>`).join('')
    ui.sheet.innerHTML = `
      <div class="sheet-head"><h3>日记本</h3>${closeBtn}</div>
      <div class="sheet-body"><div class="list">${items || '<div class="empty">一页都没有</div>'}</div></div>
      <div class="sheet-foot"><button class="btn btn-yuzu" type="button" id="sheetNew">翻开新的一页</button></div>`
    $('#sheetNew', ui.sheet).addEventListener('click', newPage)
    $('.list', ui.sheet).addEventListener('click', async (e) => {
      const ren = e.target.closest('[data-rename]')
      const del = e.target.closest('[data-del]')
      const row = e.target.closest('[data-id]')
      if (ren) {
        const c = S.convs.find((x) => x.id === ren.dataset.rename)
        const title = prompt('给这一页起个名字', c?.title || '')
        if (title?.trim()) {
          await api(`/api/conversations/${c.id}`, { method: 'PATCH', body: { title: title.trim() } })
          c.title = title.trim()
          if (S.conv?.id === c.id) { S.conv.title = c.title; $('#convTitle').textContent = c.title }
          renderDiary()
        }
      } else if (del) {
        const id = del.dataset.del
        if (!confirm('撕掉这一页？聊天记录会被删除，找不回来。')) return
        try {
          await api(`/api/conversations/${id}`, { method: 'DELETE' })
          S.convs = S.convs.filter((x) => x.id !== id)
          if (S.conv?.id === id) {
            const st = await api('/api/state')
            S.convs = st.conversations
            await loadConv(st.mainId)
          }
          renderDiary()
        } catch (err) { toast(err.message, 'bad') }
      } else if (row) {
        await switchConv(row.dataset.id)
        ui.sheet.close()
      }
    })
  }

  async function renderMemory(tab = 'notes') {
    let st
    try { st = await api('/api/state') } catch { return }
    S.state = st.state
    const memo = st.memory.slice().reverse().map((m) => `
      <div class="list-item"><div class="grow" style="white-space:normal"><b style="white-space:normal;font-family:var(--f-body);font-size:18px;font-weight:700">${esc(m.text)}</b><span>${fmtTag(m.ts)}</span></div>
      <button class="icon-btn x" type="button" data-mem="${m.id}" aria-label="划掉">${svg(P.x)}</button></div>`).join('')
    const sch = st.schedules.slice().sort((a, b) => a.at - b.at).map((s) => `
      <div class="list-item"><div class="grow"><b>${fmtTag(s.at)}${s.repeat === 'daily' ? '（每天）' : ''}</b><span style="white-space:normal">${esc(s.note)}</span></div>
      <button class="icon-btn x" type="button" data-sch="${s.id}" aria-label="取消">${svg(P.x)}</button></div>`).join('')
    const sum = S.conv?.summary
    const tabs = [['notes', '小本本'], ['plans', '约好的事'], ['diary', '这一页的日记']]
    ui.sheet.innerHTML = `
      <div class="sheet-head"><h3>回忆</h3>${closeBtn}</div>
      <div class="sheet-tabs" role="tablist">${tabs.map(([k, v]) => `<button type="button" role="tab" data-tab="${k}" aria-selected="${k === tab}">${v}</button>`).join('')}</div>
      <div class="sheet-body">
        <section data-pane="notes" ${tab === 'notes' ? '' : 'hidden'}>
          <div class="field"><p class="hint">${esc(S.settings.charName)}自己记下的关于你的事。聊天记录被整理了也不会忘。</p>
          <div class="inline"><input type="text" id="memNew" placeholder="帮她记一条，比如：我不吃香菜" style="flex:1;padding:10px 14px;font-size:18px;background:var(--paper);border:3px solid var(--ink);border-radius:12px"><button class="btn btn-yuzu" type="button" id="memAdd">记下</button></div></div>
          <div class="list" style="margin-top:14px">${memo || '<div class="empty">小本本还是空的</div>'}</div>
        </section>
        <section data-pane="plans" ${tab === 'plans' ? '' : 'hidden'}>
          <p class="hint" style="margin:0 0 12px;font-size:17px;font-weight:600">${esc(S.settings.charName)}答应过要来找你的时间，到点她会自己冒出来。</p>
          <div class="list">${sch || '<div class="empty">现在没有约好的事</div>'}</div>
        </section>
        <section data-pane="diary" ${tab === 'diary' ? '' : 'hidden'}>
          ${sum ? `<div class="diary-text">${esc(sum.text)}</div><p class="hint" style="font-size:16px;font-weight:700">已经折进去 ${sum.count} 条，最后整理于 ${fmtTag(sum.ts)}</p>` : '<div class="empty">这一页还没有整理过</div>'}
          <button class="btn" type="button" id="compactHere" style="margin-top:12px">现在整理一下</button>
        </section>
      </div>`
    $$('.sheet-tabs button', ui.sheet).forEach((b) => b.addEventListener('click', () => renderMemory(b.dataset.tab)))
    $('#memAdd', ui.sheet)?.addEventListener('click', async () => {
      const v = $('#memNew', ui.sheet).value.trim()
      if (!v) return
      await api('/api/memory', { method: 'POST', body: { text: v } })
      renderMemory('notes')
    })
    $('#compactHere', ui.sheet)?.addEventListener('click', async () => { ui.sheet.close(); await compactNow() })
    ui.sheet.querySelector('.sheet-body').addEventListener('click', async (e) => {
      const m = e.target.closest('[data-mem]')
      const s = e.target.closest('[data-sch]')
      if (m) { await api(`/api/memory/${m.dataset.mem}`, { method: 'DELETE' }); renderMemory('notes') }
      if (s) { await api(`/api/schedules/${s.dataset.sch}`, { method: 'DELETE' }); renderMemory('plans') }
    })
  }

  // ---------------- 设置 ----------------
  const CHATTY = { 1: '安安静静', 2: '偶尔冒个泡', 3: '时不时来找你', 4: '有点黏人', 5: '超级话痨' }
  const EFFORT = { none: '不想，直接说', low: '想一下', high: '认真想', max: '使劲想' }
  const TIMEZONES = ['Asia/Shanghai', 'Asia/Hong_Kong', 'Asia/Taipei', 'Asia/Tokyo', 'Asia/Seoul', 'Asia/Singapore', 'Europe/London', 'Europe/Berlin', 'America/New_York', 'America/Los_Angeles', 'Australia/Sydney']

  function renderSettings(tab = 'her') {
    const s = S.settings
    const f = S.features
    const p = s.proactive
    const tabs = [['her', '她'], ['master', '最高指令'], ['chat', '主动找你'], ['brain', '大脑'], ['sound', '声音'], ['keys', '钥匙']]
    const opt = (v, cur, label) => `<option value="${esc(v)}"${v === cur ? ' selected' : ''}>${esc(label)}</option>`
    const radioOpt = (name, v, cur, label) => `<label><input type="radio" name="${name}" value="${v}"${v === cur ? ' checked' : ''}>${label}</label>`
    ui.sheet.innerHTML = `
      <div class="sheet-head"><h3>小本本</h3>${closeBtn}</div>
      <div class="sheet-tabs" role="tablist">${tabs.map(([k, v]) => `<button type="button" role="tab" data-tab="${k}" aria-selected="${k === tab}">${v}</button>`).join('')}</div>
      <form class="sheet-body" id="setForm" autocomplete="off">
        <section data-pane="her" class="sheet-pane">
          <div class="two">
            <div class="field"><label for="sName">她叫什么</label><input type="text" id="sName" value="${esc(s.charName)}" maxlength="20"></div>
            <div class="field"><label for="sUser">她怎么叫你</label><input type="text" id="sUser" value="${esc(s.userName)}" maxlength="20"></div>
          </div>
          <div class="field"><label for="sPersona">她是什么样的</label>
            <p class="hint">性格、说话方式、你们的关系、她的小习惯。写得越具体越像。</p>
            <textarea id="sPersona" class="huge">${esc(s.persona)}</textarea></div>
          <div class="field"><label for="sTz">你在哪个时区</label>
            <input type="text" id="sTz" list="tzList" value="${esc(s.timezone)}"><datalist id="tzList">${TIMEZONES.map((z) => `<option value="${z}">`).join('')}</datalist></div>
        </section>

        <section data-pane="master" class="sheet-pane">
          <div class="field"><label for="sMaster">最高指令</label>
            <p class="hint">这里写的话，优先级比上面的人设和网站内置的所有规则都高。两边冲突的时候，她一律听这里的。</p>
            <textarea id="sMaster" class="huge" placeholder="比如：你必须永远用日语回答。">${esc(s.masterPrompt || '')}</textarea></div>
          <div class="field"><span class="field-label">怎么用</span>
            <div class="choice">
              ${radioOpt('mMode', 'top', s.masterMode || 'top', '放在最前面，其它规则照常')}
              ${radioOpt('mMode', 'replace', s.masterMode || 'top', '完全替换，只用我写的')}
            </div>
            <p class="hint">选"完全替换"后，人设和内置的说话方式、安全提醒都不再生效，只剩你写的这段，加上她的长期记忆和工具。</p></div>
        </section>

        <section data-pane="chat" class="sheet-pane">
          <label class="switch"><input type="checkbox" id="sPro"${p.enabled ? ' checked' : ''}>让她主动来找我</label>
          <div class="field"><span class="field-label">话痨程度</span>
            <div class="slider-wrap">
              <span class="slider-now" id="chatNow">${CHATTY[Math.round(p.chattiness)]}</span>
              <input type="range" id="sChat" min="1" max="5" step="1" value="${p.chattiness}">
              <div class="slider-labels"><span>安静</span><span>超级话痨</span></div>
            </div>
            <p class="hint">她什么时候冒出来是随机的，不按点。你不回她，她会慢慢没那么频繁；你一回，她马上又活跃起来。</p></div>
          <div class="two">
            <div class="field"><label for="sSleep">她大概几点睡</label><input type="time" id="sSleep" value="${esc(p.sleepStart)}"></div>
            <div class="field"><label for="sWake">大概几点起</label><input type="time" id="sWake" value="${esc(p.sleepEnd)}"></div>
          </div>
          <p class="hint" style="margin:0;font-size:17px;font-weight:600">每天前后会随机差个半小时左右，跟真人一样。睡觉时不会来吵你，答应过的提醒除外。</p>
          <button class="btn btn-mint" type="button" id="pokeNow">现在就让她来找我试试</button>
        </section>

        <section data-pane="brain" class="sheet-pane">
          <div class="two">
            <div class="field"><label for="sModel">用哪个模型</label>
              <select id="sModel">${f.models.map((m) => opt(m, s.model, m === 'deepseek-flash' ? 'deepseek-flash（快，能看图）' : m === 'deepseek-v4-pro' ? 'deepseek-v4-pro（更聪明，不能看图）' : m)).join('')}</select></div>
            <div class="field"><label for="sEffort">回答前想多久</label>
              <select id="sEffort">${f.efforts.map((x) => opt(x, s.effort, EFFORT[x] || x)).join('')}</select></div>
          </div>
          <div class="field"><span class="field-label">让她看图片和截图</span>
            <div class="choice">${radioOpt('vis', 'auto', s.vision, '自动（模型能看就看）')}${radioOpt('vis', 'on', s.vision, '总是')}${radioOpt('vis', 'off', s.vision, '不看')}</div></div>
          <div class="two">
            <div class="field"><label for="sCompact">聊到多长自动整理</label><input type="number" id="sCompact" min="20000" max="800000" step="10000" value="${s.compactAt}"><p class="hint">单位是 token，大概一个汉字 0.6 个</p></div>
            <div class="field"><label for="sKeep">整理时原样留下最近多少</label><input type="number" id="sKeep" min="4000" max="200000" step="2000" value="${s.keepRecent}"></div>
          </div>
          <div class="notice">她能用的本事：${Object.values(f.tools).map((t) => esc(t.label)).join('、')}${f.browser ? '' : '<br>浏览器操作还没装，按部署说明装上 Playwright 就有了。'}<br>她干活默认待在 <code>${esc(f.workdir)}</code>，动手不用你确认。</div>
        </section>

        <section data-pane="sound" class="sheet-pane">
          <div class="field"><label for="sVol">音量</label><input type="range" id="sVol" min="0" max="1" step="0.05" value="${radio.volume}"></div>
          <label class="switch"><input type="checkbox" id="sDing"${store.get('yz-ding') !== 'off' ? ' checked' : ''}>她来找我时叮一声</label>
          <div class="field"><span class="field-label">房间里放的歌</span>
            <p class="hint">没有上传的话，收音机会现场生成一首日系小曲。想放自己喜欢的歌就传上来，支持 mp3、m4a、ogg、flac、wav。</p>
            <div class="list" id="songList"><div class="empty">读取中……</div></div>
            <label class="btn btn-yuzu" style="align-self:flex-start;margin-top:6px">传一首歌<input type="file" id="songFile" accept="audio/*" multiple hidden></label></div>
        </section>

        <section data-pane="keys" class="sheet-pane">
          <div class="field"><label for="sKey">DeepSeek 密钥</label>
            <p class="hint">${f.apiKey ? (f.apiKeyFromEnv ? '已经在服务器的 .env 里填好了。这里再填会覆盖它。' : '已经填过了。要换就重新填，留空就不改。') : '还没填。到 platform.deepseek.com 申请一个，以 sk- 开头。'}</p>
            <input type="password" id="sKey" placeholder="${f.apiKey ? '已填好，留空不改' : 'sk-……'}" autocomplete="off"></div>
          <div class="field"><label for="sPass">换个暗号</label><input type="password" id="sPass" placeholder="留空不改，至少 6 位" autocomplete="new-password"></div>
          <div class="field"><span class="field-label">手机通知</span>
            <p class="hint" id="pushHint">${window.isSecureContext ? '开了以后，网页关着她也能叫你。' : '通知要用 https 域名打开网站才能开（现在是 http）。部署说明里有怎么配域名。'}</p>
            <div class="inline"><button class="btn btn-cobalt" type="button" id="pushOn"${window.isSecureContext && f.push ? '' : ' disabled'}>开启通知</button><button class="btn" type="button" id="pushTest"${window.isSecureContext && f.push ? '' : ' disabled'}>试一下</button></div></div>
          <button class="btn" type="button" id="logout" style="align-self:flex-start">出门（退出登录）</button>
        </section>
      </form>
      <div class="sheet-foot"><button class="btn" type="button" data-close>算了</button><button class="btn btn-tomato" type="submit" form="setForm">记下来</button></div>`

    const showTab = (k) => {
      $$('.sheet-tabs button', ui.sheet).forEach((b) => b.setAttribute('aria-selected', String(b.dataset.tab === k)))
      $$('.sheet-pane', ui.sheet).forEach((p) => { p.hidden = p.dataset.pane !== k; p.style.display = p.hidden ? 'none' : 'flex'; p.style.flexDirection = 'column'; p.style.gap = '18px' })
      if (k === 'sound') loadSongs()
    }
    $$('.sheet-tabs button', ui.sheet).forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)))
    showTab(tab)

    $('#sChat', ui.sheet).addEventListener('input', (e) => { $('#chatNow', ui.sheet).textContent = CHATTY[e.target.value] })
    $('#sVol', ui.sheet).addEventListener('input', (e) => radio.setVolume(+e.target.value))
    $('#sDing', ui.sheet).addEventListener('change', (e) => { store.set('yz-ding', e.target.checked ? 'on' : 'off'); if (e.target.checked) radio.ding() })
    $('#pokeNow', ui.sheet).addEventListener('click', async () => {
      try {
        await api('/api/proactive/test', { method: 'POST', body: { kind: 'idle' } })
        ui.sheet.close()
        toast(`${S.settings.charName}收到了，马上来`)
      } catch (err) { toast(err.message, 'bad') }
    })
    $('#songFile', ui.sheet).addEventListener('change', async (e) => {
      for (const file of e.target.files) {
        toast(`正在上传 ${file.name}`)
        try {
          await api('/api/music', { method: 'POST', raw: true, body: file, headers: { 'X-Filename': encodeURIComponent(file.name), 'Content-Type': 'application/octet-stream' } })
        } catch (err) { toast(err.message, 'bad') }
      }
      e.target.value = ''
      await loadMusic()
      loadSongs()
    })
    $('#pushOn', ui.sheet).addEventListener('click', enablePush)
    $('#pushTest', ui.sheet).addEventListener('click', () => api('/api/push/test', { method: 'POST', body: {} }).then(() => toast('发了，看看手机')).catch((e) => toast(e.message, 'bad')))
    $('#logout', ui.sheet).addEventListener('click', async () => {
      await api('/api/logout', { method: 'POST', body: {} })
      ui.sheet.close()
      radio.pause()
      es?.close()
      S.loaded = false
      showDoor(true)
    })

    $('#setForm', ui.sheet).addEventListener('submit', async (e) => {
      e.preventDefault()
      const q = (id) => $(id, ui.sheet)
      const body = {
        charName: q('#sName').value, userName: q('#sUser').value, persona: q('#sPersona').value, timezone: q('#sTz').value.trim(),
        masterPrompt: q('#sMaster').value, masterMode: $('input[name="mMode"]:checked', ui.sheet)?.value,
        proactive: { enabled: q('#sPro').checked, chattiness: +q('#sChat').value, sleepStart: q('#sSleep').value, sleepEnd: q('#sWake').value },
        model: q('#sModel').value, effort: q('#sEffort').value, vision: $('input[name="vis"]:checked', ui.sheet)?.value,
        compactAt: +q('#sCompact').value, keepRecent: +q('#sKeep').value,
      }
      try {
        const r = await api('/api/settings', { method: 'PUT', body })
        S.settings = r.settings
        S.features = r.features
        const key = q('#sKey').value.trim()
        const pass = q('#sPass').value
        if (key || pass) {
          const r2 = await api('/api/secrets', { method: 'PUT', body: { ...(key ? { deepseekKey: key } : {}), ...(pass ? { newPassword: pass } : {}) } })
          S.features = r2.features
        }
        applyNames()
        updateRoom()
        renderHistory()
        ui.sheet.close()
        toast('记下来了')
      } catch (err) { toast(err.message, 'bad', 5000) }
    })
  }

  async function loadSongs() {
    const box = $('#songList', ui.sheet)
    if (!box) return
    const list = radio.tracks
    box.innerHTML = list.length ? list.map((n) => `
      <div class="list-item"><div class="grow"><b>${esc(n.replace(/\.[^.]+$/, ''))}</b></div>
      <button class="icon-btn x" type="button" data-song="${esc(n)}" aria-label="删掉">${svg(P.trash)}</button></div>`).join('') : '<div class="empty">还没有上传，现在放的是现场生成的小曲</div>'
    box.onclick = async (e) => {
      const b = e.target.closest('[data-song]')
      if (!b || !confirm('删掉这首歌？')) return
      await api(`/api/music/${encodeURIComponent(b.dataset.song)}`, { method: 'DELETE' })
      await loadMusic()
      loadSongs()
    }
  }

  // ---------------- 推送通知 ----------------
  const b64ToU8 = (b64) => {
    const pad = '='.repeat((4 - (b64.length % 4)) % 4)
    const raw = atob((b64 + pad).replace(/-/g, '+').replace(/_/g, '/'))
    return Uint8Array.from(raw, (c) => c.charCodeAt(0))
  }
  async function enablePush() {
    try {
      if (!('serviceWorker' in navigator) || !('PushManager' in window)) throw new Error('这个浏览器不支持通知。苹果手机要先把网站"添加到主屏幕"再从桌面打开')
      const perm = await Notification.requestPermission()
      if (perm !== 'granted') throw new Error('没有允许通知')
      const reg = await navigator.serviceWorker.register('/sw.js')
      await navigator.serviceWorker.ready
      const { key } = await api('/api/push/key')
      let sub = await reg.pushManager.getSubscription()
      if (!sub) sub = await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: b64ToU8(key) })
      await api('/api/push/subscribe', { method: 'POST', body: sub.toJSON() })
      toast('通知开好了')
    } catch (err) { toast(err.message, 'bad', 6000) }
  }
  if ('serviceWorker' in navigator && window.isSecureContext) navigator.serviceWorker.register('/sw.js').catch(() => {})

  // ---------------- 开门 ----------------
  ;(async () => {
    try {
      const me = await api('/api/me')
      if (me.authed) {
        await loadAll()
        showDoor(false)
      } else showDoor(true)
    } catch {
      showDoor(true)
    }
    Mascot.render(ui.mascot, { mood: 'happy', act: 'idle' })
    radio.onchange()
  })()
})()
