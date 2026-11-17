;(() => {
  const $ = (s, el = document) => el.querySelector(s)
  const $$ = (s, el = document) => [...el.querySelectorAll(s)]

  const S = {
    settings: null, state: {}, features: { tools: {} }, convs: [], conv: null,
    running: new Set(), drafts: new Map(), runningTools: new Map(),
    memory: [], schedules: [], showOlder: false, attachments: [], compacting: false,
    loaded: false, sending: false,
  }
  const ui = {
    app: $('#app'), history: $('#history'), live: $('#live'), scroller: $('#scroller'), input: $('#input'),
    send: $('#sendBtn'), sheet: $('#sheet'), toasts: $('#toasts'), jump: $('#jumpBtn'), status: $('#status'),
  }

  // ---------------- 小工具 ----------------
  const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c])
  const store = {
    get(k) { try { return localStorage.getItem(k) } catch { return null } },
    set(k, v) { try { localStorage.setItem(k, v) } catch {} },
  }

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
      showLogin()
      throw new Error('请先输入暗号')
    }
    if (!res.ok) throw Object.assign(new Error(data?.error || `出错了（${res.status}）`), { status: res.status })
    return data
  }

  function toast(text, kind = '', ms = 3000, onclick) {
    const el = document.createElement('div')
    el.className = `toast ${kind}${onclick ? ' click' : ''}`
    el.textContent = text
    if (onclick) el.addEventListener('click', () => { onclick(); el.remove() })
    ui.toasts.append(el)
    setTimeout(() => el.remove(), ms)
  }

  // 防连点：按钮在请求期间失效
  async function once(btn, fn) {
    if (btn.dataset.busy) return
    btn.dataset.busy = '1'
    btn.disabled = true
    try { return await fn() } finally { delete btn.dataset.busy; btn.disabled = false }
  }

  const tz = () => S.settings?.timezone || 'Asia/Shanghai'
  function parts(ts) {
    const f = new Intl.DateTimeFormat('zh-CN', { timeZone: tz(), year: 'numeric', month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit', hour12: false })
    const p = Object.fromEntries(f.formatToParts(new Date(ts)).map((x) => [x.type, x.value]))
    return { y: +p.year, mo: +p.month, d: +p.day, h: +p.hour % 24, mi: p.minute }
  }
  const sameDay = (a, b) => a.y === b.y && a.mo === b.mo && a.d === b.d
  function fmtTime(ts) {
    const p = parts(ts)
    const now = parts(Date.now())
    const clock = `${String(p.h).padStart(2, '0')}:${p.mi}`
    if (sameDay(p, now)) return clock
    if (sameDay(p, parts(Date.now() - 864e5))) return `昨天 ${clock}`
    return `${p.y !== now.y ? `${p.y}年` : ''}${p.mo}月${p.d}日 ${clock}`
  }
  function fmtShort(ts) {
    const p = parts(ts)
    const now = parts(Date.now())
    if (sameDay(p, now)) return `${String(p.h).padStart(2, '0')}:${p.mi}`
    if (sameDay(p, parts(Date.now() - 864e5))) return '昨天'
    return `${p.mo}/${p.d}`
  }

  // ---------------- iPad 键盘：页面高度跟着可见区域走 ----------------
  const vv = window.visualViewport
  function fitViewport() {
    const h = vv ? vv.height : window.innerHeight
    document.documentElement.style.setProperty('--app-h', `${Math.round(h)}px`)
    if (vv && (vv.offsetTop > 0 || window.scrollY > 0)) window.scrollTo(0, 0)
  }
  vv?.addEventListener('resize', fitViewport)
  vv?.addEventListener('scroll', fitViewport)
  window.addEventListener('resize', fitViewport)
  fitViewport()

  // ---------------- 文字渲染 ----------------
  function inline(text) {
    let s = esc(text)
    s = s.replace(/`([^`\n]+)`/g, '<code>$1</code>')
    s = s.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>')
    s = s.replace(/\[([^\]\n]+)\]\((https?:\/\/[^\s)]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>')
    s = s.replace(/(^|[\s(（])(https?:\/\/[^\s<)）]+)/g, '$1<a href="$2" target="_blank" rel="noopener">$2</a>')
    return s.replace(/\n/g, '<br>')
  }
  function bubbles(text) {
    const out = []
    const re = /```(\w*)[^\n]*\n?([\s\S]*?)(```|$)/g
    let last = 0
    let m
    const pushText = (t) => {
      for (const para of t.split(/\n\s*\n/)) if (para.trim()) out.push({ html: inline(para.trim()) })
    }
    while ((m = re.exec(String(text || '')))) {
      pushText(text.slice(last, m.index))
      out.push({ code: true, html: `${m[1] ? `<span class="lang">${esc(m[1])}</span>` : ''}<pre><code>${esc(m[2].replace(/\n$/, ''))}</code></pre>` })
      last = re.lastIndex
      if (!m[3]) break
    }
    pushText(String(text || '').slice(last))
    return out
  }
  const bubblesHtml = (text, cursor) => {
    const bs = bubbles(text)
    return bs.map((b, i) => `<div class="bubble${b.code ? ' codeblock' : ''}${cursor && i === bs.length - 1 && !b.code ? ' cursor' : ''}">${b.html}</div>`).join('')
  }

  // ---------------- 图标 ----------------
  const P = {
    bash: 'M4 5h16v14H4zM8 10l3 2-3 2M13 15h4',
    file: 'M6 3h9l4 4v14H6zM14 3v5h5',
    edit: 'M4 20l4-1 11-11-3-3L5 16z',
    find: 'M10.5 17a6.5 6.5 0 1 1 0-13 6.5 6.5 0 0 1 0 13zM15.5 15.5L20 20',
    pulse: 'M3 12h4l2-5 4 10 2-5h6',
    globe: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM3 12h18M12 3c3 3.5 3 14.5 0 18M12 3c-3 3.5-3 14.5 0 18',
    note: 'M6 4h10a2 2 0 0 1 2 2v14H8a2 2 0 0 1-2-2zM10 8h4',
    clock: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM12 7v5l3 2',
    mail: 'M3 6h18v12H3zM3 6l9 7 9-7',
    x: 'M6 6l12 12M18 6L6 18',
    trash: 'M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13',
    pen: 'M4 20l4-1 11-11-3-3L5 16z',
  }
  const svg = (d) => `<svg viewBox="0 0 24 24"><path d="${d}"/></svg>`
  const TOOL_ICON = {
    bash: 'bash', read: 'file', write: 'file', edit: 'edit', glob: 'find', grep: 'find', sysinfo: 'pulse', webfetch: 'globe',
  }
  const NOTE_TOOLS = new Set(['remember', 'forget', 'schedule_message', 'list_schedules', 'cancel_schedule', 'set_mood'])

  // ---------------- 增量渲染：每块有固定 key，没变的不动 ----------------
  const tpl = document.createElement('template')
  function patch(container, blocks, animate) {
    const old = new Map()
    for (const el of container.children) old.set(el.dataset.key, el)
    let prev = null
    for (const b of blocks) {
      let el = old.get(b.key)
      if (el && el._html !== b.html) {
        const n = make(b)
        const was = $$('details', el).map((d) => d.open)
        $$('details', n).forEach((d, i) => { if (was[i] !== undefined) d.open = was[i] })
        el.replaceWith(n)
        el = n
      } else if (!el) {
        el = make(b)
        if (animate) el.classList.add('enter')
      }
      old.delete(b.key)
      const want = prev ? prev.nextElementSibling : container.firstElementChild
      if (el !== want) container.insertBefore(el, want)
      prev = el
    }
    for (const el of old.values()) el.remove()
  }
  function make(b) {
    tpl.innerHTML = b.html.trim()
    const el = tpl.content.firstElementChild
    el.dataset.key = b.key
    el._html = b.html
    return el
  }

  // ---------------- 滚动：只有你在底部时才自动跟随 ----------------
  let stick = true
  let touching = false
  let userScrollAt = 0
  const distBottom = () => ui.scroller.scrollHeight - ui.scroller.scrollTop - ui.scroller.clientHeight
  ui.scroller.addEventListener('touchstart', () => { touching = true; userScrollAt = Date.now() }, { passive: true })
  ui.scroller.addEventListener('touchend', () => { touching = false; userScrollAt = Date.now() }, { passive: true })
  ui.scroller.addEventListener('wheel', () => { userScrollAt = Date.now() }, { passive: true })
  ui.scroller.addEventListener('scroll', () => {
    const userish = touching || Date.now() - userScrollAt < 800
    if (userish) stick = distBottom() < 30
    else if (distBottom() < 30) stick = true
    if (stick) ui.jump.hidden = true
  }, { passive: true })
  function follow(force) {
    if (force) stick = true
    if (stick && !touching && Date.now() - userScrollAt > 800) {
      ui.scroller.scrollTop = ui.scroller.scrollHeight
      ui.jump.hidden = true
    } else if (!stick) ui.jump.hidden = false
  }
  ui.jump.addEventListener('click', () => { userScrollAt = 0; follow(true) })

  // ---------------- 聊天记录 ----------------
  const parseArgs = (s) => { try { return JSON.parse(s || '{}') } catch { return {} } }
  function toolSummary(name, a) {
    switch (name) {
      case 'bash': return a.command
      case 'read': case 'write': case 'edit': return a.path
      case 'glob': case 'grep': return `${a.pattern}${a.path ? `  ${a.path}` : ''}`
      case 'webfetch': case 'browser_open': return a.url
      case 'browser_click': return `[${a.ref}]`
      case 'browser_type': return a.text
      case 'browser_scroll': return { down: '往下', up: '往上', top: '到顶', bottom: '到底' }[a.direction] || ''
      default: return ''
    }
  }
  function toolState(call, result) {
    if (!result) return S.runningTools.has(call.id) || (S.conv && S.running.has(S.conv.id)) ? ['run', '进行中'] : ['bad', '中断']
    const t = result.content || ''
    if (/^(错误|出错了)/.test(t)) return ['bad', '出错']
    const code = /\[退出码 (\d+)\]\s*$/.exec(t)
    if (code && code[1] !== '0') return ['bad', `退出码 ${code[1]}`]
    if (/\[超过 \d+ 秒被停止了/.test(t)) return ['bad', '超时']
    return ['ok', '完成']
  }
  const clipText = (t, n) => (t.length > n ? `${t.slice(0, n)}\n（后面还有 ${t.length - n} 字）` : t)

  function noteLine(call, result) {
    const a = parseArgs(call.arguments)
    const n = call.name
    if (n === 'set_mood' || n === 'list_schedules') return ''
    let text = ''
    if (n === 'remember') text = `记下了：${a.text || ''}`
    else if (n === 'forget') text = '划掉了一条记忆'
    else if (n === 'schedule_message') text = `约好了 ${a.at || `${a.in_minutes} 分钟后`}：${a.note || ''}`
    else if (n === 'cancel_schedule') text = '取消了一个约定'
    if (!result) text += '（进行中）'
    return `<span class="note-line">${svg(n === 'schedule_message' || n === 'cancel_schedule' ? P.clock : P.note)}${esc(text)}</span>`
  }

  function actCard(call, result) {
    if (NOTE_TOOLS.has(call.name)) return noteLine(call, result)
    const name = call.name
    const meta = S.features.tools?.[name] || { label: name }
    const a = parseArgs(call.arguments)
    const [cls, label] = toolState(call, result)
    let body = ''
    if (name === 'bash') body += `<span class="label">命令</span><pre>${esc(a.command || '')}</pre>`
    else if (name === 'write') body += `<span class="label">写入 ${esc(a.path)}</span><pre>${esc(clipText(String(a.content || ''), 4000))}</pre>`
    else if (name === 'edit') body += `<span class="label">修改 ${esc(a.path)}</span><pre>${esc(`- ${clipText(String(a.oldString || ''), 2000)}\n+ ${clipText(String(a.newString || ''), 2000)}`)}</pre>`
    else if (Object.keys(a).length) body += `<span class="label">参数</span><pre>${esc(JSON.stringify(a, null, 2))}</pre>`
    if (result) body += `<span class="label">结果</span><pre>${esc(clipText(result.content || '', 12000))}</pre>`
    const shots = result?.images?.length ? `<div class="pics shots">${result.images.map((id) => `<img src="/api/files/${encodeURIComponent(id)}" alt="截图" loading="lazy">`).join('')}</div>` : ''
    return `<details class="act"><summary><span class="act-ic">${svg(P[TOOL_ICON[name]] || P.globe)}</span><span class="act-label">${esc(meta.label || name)}</span><code class="act-sum">${esc(toolSummary(name, a))}</code><span class="act-state ${cls}">${label}</span></summary><div class="act-body">${body}</div></details>${shots}`
  }

  const TRIGGER = { idle: '主动来找你', wake: '刚醒来找你', reminder: '按约好的时间来找你' }

  function buildBlocks() {
    const c = S.conv
    const blocks = []
    if (!c) return blocks
    const results = new Map(c.messages.filter((m) => m.role === 'tool').map((m) => [m.tool_call_id, m]))
    const archived = c.messages.filter((m) => m.archived && !m.hidden && (m.role === 'user' || (m.role === 'assistant' && m.content))).length
    if (archived && !S.showOlder) {
      blocks.push({ key: 'older', html: `<div class="blk sep">更早的 ${archived} 条已整理成日记 <button type="button" data-act="older">展开</button><button type="button" data-act="diary">看日记</button></div>` })
    }
    let lastTs = 0
    let lastSide = ''
    let trigger = null
    for (const m of c.messages) {
      if (m.archived && !S.showOlder) continue
      if (m.role === 'tool') continue
      if (m.role === 'user' && m.hidden) { trigger = m.trigger || 'idle'; continue }
      if (m.role === 'event') {
        let inner = esc(m.content)
        if (m.kind === 'compact') inner += ' <button type="button" data-act="diary">看日记</button>'
        blocks.push({ key: m.id, html: `<div class="blk sep${m.kind === 'error' ? ' error' : ''}">${inner}</div>` })
        lastSide = ''
        continue
      }
      if (m.ts - lastTs > 10 * 60 * 1000) {
        blocks.push({ key: `t-${m.id}`, html: `<div class="blk time-sep">${fmtTime(m.ts)}</div>` })
        lastSide = ''
      }
      lastTs = m.ts
      if (m.role === 'user') {
        trigger = null
        const pics = m.images?.length ? `<div class="pics">${m.images.map((id) => `<img src="/api/files/${encodeURIComponent(id)}" alt="图片" loading="lazy">`).join('')}</div>` : ''
        blocks.push({ key: m.id, html: `<div class="blk${lastSide === 'me' ? ' cont' : ''}"><div class="row me">${pics}${bubblesHtml(m.content)}</div></div>` })
        lastSide = 'me'
      } else if (m.role === 'assistant') {
        let inner = ''
        if (trigger) {
          inner += `<span class="note-line accent">${svg(P.mail)}${TRIGGER[trigger] || TRIGGER.idle}</span>`
          trigger = null
        }
        if (m.reasoning_content) inner += `<details class="thought"><summary>心里话</summary><div class="thought-body">${esc(m.reasoning_content)}</div></details>`
        inner += bubblesHtml(m.content)
        for (const call of m.tool_calls || []) inner += actCard(call, results.get(call.id))
        if (!inner) continue
        blocks.push({ key: m.id, html: `<div class="blk${lastSide === 'her' ? ' cont' : ''}"><div class="row her">${inner}</div></div>` })
        lastSide = 'her'
      }
    }
    if (!c.messages.some((m) => !m.hidden && m.role !== 'event')) blocks.push({ key: 'empty', html: '<div class="blk empty-hint">还没有聊天，说点什么吧</div>' })
    if (!S.features.apiKey) blocks.push({ key: 'nokey', html: '<div class="blk banner"><span>还没有填 DeepSeek 密钥，暂时没法回复</span><button class="btn" type="button" data-act="key">去填写</button></div>' })
    return blocks
  }

  let histQueued = false
  let histAnimate = false
  function renderHistory(animate = true) {
    histAnimate = histAnimate || animate
    if (histQueued) return
    histQueued = true
    requestAnimationFrame(() => {
      histQueued = false
      patch(ui.history, buildBlocks(), histAnimate)
      histAnimate = false
      follow()
    })
  }

  let liveQueued = false
  function renderLive() {
    if (liveQueued) return
    liveQueued = true
    requestAnimationFrame(() => {
      liveQueued = false
      const c = S.conv
      const blocks = []
      if (c && S.running.has(c.id)) {
        const drafts = [...S.drafts.entries()].filter(([, d]) => d.convId === c.id)
        for (const [id, d] of drafts) {
          let inner = ''
          if (d.reasoning && !d.content) inner += `<div class="thinking">${esc(d.reasoning.slice(-600))}</div>`
          inner += d.content ? bubblesHtml(d.content, true) : (d.reasoning ? '' : '<div class="bubble"><span class="typing"><i></i><i></i><i></i></span></div>')
          blocks.push({ key: `d-${id}`, html: `<div class="blk"><div class="row her">${inner}</div></div>` })
        }
        if (!drafts.length && !S.runningTools.size) blocks.push({ key: 'typing', html: '<div class="blk"><div class="row her"><div class="bubble"><span class="typing"><i></i><i></i><i></i></span></div></div></div>' })
      }
      if (S.compacting) blocks.push({ key: 'compacting', html: '<div class="blk sep">正在整理前面的聊天</div>' })
      patch(ui.live, blocks, false)
      $$('.thinking', ui.live).forEach((el) => { el.scrollTop = el.scrollHeight })
      follow()
      updateStatus()
    })
  }

  function updateStatus() {
    const c = S.conv
    let text = '在线'
    let busy = false
    if (c && S.running.has(c.id)) {
      busy = true
      if (S.runningTools.size) text = `正在${[...S.runningTools.values()].pop() || '操作'}`
      else if ([...S.drafts.values()].some((d) => d.convId === c.id && d.content)) text = '正在输入'
      else text = '正在想'
    } else if (S.state?.asleep) text = '睡着了'
    if (ui.status.textContent !== text) ui.status.textContent = text
    ui.status.classList.toggle('busy', busy)
    const running = c && S.running.has(c.id)
    ui.send.classList.toggle('stop', !!running)
    ui.send.textContent = running ? '停止' : '发送'
  }

  ui.history.addEventListener('click', (e) => {
    const b = e.target.closest('[data-act]')
    if (!b) return
    if (b.dataset.act === 'older') { S.showOlder = true; renderHistory(false) }
    if (b.dataset.act === 'diary') openSheet('memory', 'diary')
    if (b.dataset.act === 'key') openSheet('settings', 'account')
  })
  document.addEventListener('click', (e) => {
    const img = e.target.closest('.pics img')
    if (!img) return
    const v = document.createElement('div')
    v.className = 'viewer'
    v.innerHTML = `<img src="${img.src}" alt="">`
    v.addEventListener('click', () => v.remove())
    document.body.append(v)
  })

  // ---------------- 侧栏 ----------------
  function renderConvList() {
    const list = $('#convList')
    list.innerHTML = S.convs.map((c) => `
      <div class="conv${c.id === S.conv?.id ? ' current' : ''}" data-id="${esc(c.id)}" role="button" tabindex="0">
        <div class="conv-main">
          <div class="conv-top"><span class="conv-title">${esc(c.title)}</span><span class="conv-time">${fmtShort(c.updatedAt)}</span></div>
          <span class="conv-preview">${esc((c.preview || '空白的一页').replace(/[`*#]/g, '').replace(/\s+/g, ' '))}</span>
        </div>
        <div class="conv-actions">
          <button class="icon-btn" type="button" data-rename="${esc(c.id)}" aria-label="改名">${svg(P.pen)}</button>
          <button class="icon-btn" type="button" data-del="${esc(c.id)}" aria-label="删除">${svg(P.trash)}</button>
        </div>
        ${c.unread && c.id !== S.conv?.id ? `<span class="conv-unread">${c.unread}</span>` : ''}
      </div>`).join('')
    const others = S.convs.some((c) => c.id !== S.conv?.id && c.unread)
    $('#menuDot').hidden = !others
  }

  $('#convList').addEventListener('click', async (e) => {
    const ren = e.target.closest('[data-rename]')
    const del = e.target.closest('[data-del]')
    const row = e.target.closest('[data-id]')
    if (ren) {
      e.stopPropagation()
      const c = S.convs.find((x) => x.id === ren.dataset.rename)
      const title = prompt('改个名字', c?.title || '')
      if (!title?.trim()) return
      await api(`/api/conversations/${c.id}`, { method: 'PATCH', body: { title: title.trim() } })
      c.title = title.trim()
      if (S.conv?.id === c.id) { S.conv.title = c.title; $('#convTitle').textContent = c.title }
      renderConvList()
    } else if (del) {
      e.stopPropagation()
      if (!confirm('删除这一页？聊天记录会被删掉，找不回来。')) return
      try {
        await api(`/api/conversations/${del.dataset.del}`, { method: 'DELETE' })
        const st = await api('/api/state')
        S.convs = st.conversations
        if (!S.convs.some((c) => c.id === S.conv?.id)) await loadConv(st.mainId)
        renderConvList()
      } catch (err) { toast(err.message, 'bad') }
    } else if (row && row.dataset.id !== S.conv?.id) {
      await loadConv(row.dataset.id)
      closeDrawer()
    } else if (row) closeDrawer()
  })

  const closeDrawer = () => ui.app.classList.remove('drawer')
  $('#menuBtn').addEventListener('click', () => ui.app.classList.toggle('drawer'))
  $('#scrim').addEventListener('click', closeDrawer)

  $('#newPageBtn').addEventListener('click', (e) => once(e.currentTarget, async () => {
    try {
      const c = await api('/api/conversations', { method: 'POST', body: {} })
      S.convs.unshift({ id: c.id, title: c.title, createdAt: c.createdAt, updatedAt: c.updatedAt, unread: 0, preview: '' })
      await loadConv(c.id)
      closeDrawer()
      ui.input.focus()
    } catch (err) { toast(err.message, 'bad') }
  }))

  $('#compactBtn').addEventListener('click', (e) => once(e.currentTarget, compactNow))
  async function compactNow() {
    if (!S.conv) return
    if (!confirm('把前面的聊天整理成一页日记？最近的几句会原样保留。')) return
    S.compacting = true
    renderLive()
    try {
      const r = await api(`/api/conversations/${S.conv.id}/compact`, { method: 'POST', body: {} })
      if (!r.ok) toast(r.reason)
    } catch (err) { toast(err.message, 'bad') }
    S.compacting = false
    renderLive()
  }

  // ---------------- 音乐 ----------------
  const radio = window.Radio
  const musicBtn = $('#musicBtn')
  radio.onchange = () => {
    musicBtn.setAttribute('aria-pressed', String(radio.playing))
    $('span', musicBtn).textContent = radio.playing ? '暂停' : '音乐'
  }
  musicBtn.addEventListener('click', () => {
    radio.toggle()
    store.set('yz-music', radio.playing ? 'on' : 'off')
  })
  async function loadMusic() {
    try { radio.setTracks(await api('/api/music')) } catch {}
  }
  // 浏览器要求先有一次点击才能出声，所以在第一次点按时开始放
  const startMusicOnce = () => {
    document.removeEventListener('pointerdown', startMusicOnce, true)
    if (store.get('yz-music') !== 'off' && !radio.playing) radio.play()
  }

  // ---------------- 登录 ----------------
  function showLogin() {
    ui.app.hidden = true
    $('#login').hidden = false
    es?.close()
    S.loaded = false
    setTimeout(() => $('#loginPass').focus(), 50)
  }
  $('#loginForm').addEventListener('submit', async (e) => {
    e.preventDefault()
    const btn = $('button', e.currentTarget)
    await once(btn, async () => {
      $('#loginErr').textContent = ''
      try {
        await api('/api/login', { method: 'POST', body: { password: $('#loginPass').value } })
        $('#loginPass').value = ''
        await enter()
      } catch (err) { $('#loginErr').textContent = err.message }
    })
  })

  async function enter() {
    await loadAll()
    $('#login').hidden = true
    ui.app.hidden = false
    fitViewport()
    follow(true)
    document.addEventListener('pointerdown', startMusicOnce, true)
  }

  // ---------------- 数据 ----------------
  async function loadAll() {
    const st = await api('/api/state')
    S.settings = st.settings
    S.state = st.state
    S.features = st.features
    S.convs = st.conversations
    S.running = new Set(st.running)
    S.memory = st.memory
    S.schedules = st.schedules
    S.loaded = true
    await loadConv(st.mainId)
    loadMusic()
    connectEvents()
  }

  async function refreshState() {
    try {
      const st = await api('/api/state')
      S.state = st.state
      S.convs = st.conversations
      S.running = new Set(st.running)
      S.memory = st.memory
      S.schedules = st.schedules
      renderConvList()
      updateStatus()
    } catch {}
  }

  async function loadConv(id) {
    const c = await api(`/api/conversations/${encodeURIComponent(id)}`)
    S.conv = c
    S.showOlder = false
    S.drafts.clear()
    $('#convTitle').textContent = c.title
    ui.history.replaceChildren()
    ui.live.replaceChildren()
    patch(ui.history, buildBlocks(), false)
    renderLive()
    renderConvList()
    follow(true)
    document.fonts?.ready.then(() => follow())
    markRead()
  }

  async function reloadConv() {
    if (!S.conv) return
    try {
      const c = await api(`/api/conversations/${S.conv.id}`)
      if (c.id !== S.conv.id) return
      S.conv = c
      $('#convTitle').textContent = c.title
      renderHistory(false)
      renderLive()
    } catch {}
  }

  function markRead() {
    const c = S.conv
    if (!c || document.hidden || ui.app.hidden) return
    const item = S.convs.find((x) => x.id === c.id)
    if (c.unread || item?.unread) {
      c.unread = 0
      if (item) item.unread = 0
      renderConvList()
      api(`/api/conversations/${c.id}/read`, { method: 'POST', body: {} }).catch(() => {})
    }
  }

  let convsTimer = null
  const refreshConvs = () => {
    clearTimeout(convsTimer)
    convsTimer = setTimeout(async () => {
      try { S.convs = await api('/api/conversations'); renderConvList() } catch {}
    }, 500)
  }

  // ---------------- 实时事件 ----------------
  let es = null
  let titleTimer = null
  function flashTitle() {
    if (titleTimer || !document.hidden) return
    let on = false
    titleTimer = setInterval(() => { document.title = (on = !on) ? '(新消息) 小屋' : '小屋' }, 1200)
  }
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) return
    clearInterval(titleTimer)
    titleTimer = null
    document.title = '小屋'
    markRead()
  })

  function connectEvents() {
    es?.close()
    es = new EventSource('/api/events')
    let opened = false
    es.onopen = () => {
      if (opened) { refreshState(); reloadConv() }
      opened = true
    }
    es.onmessage = (e) => {
      let ev
      try { ev = JSON.parse(e.data) } catch { return }
      handle(ev)
    }
  }

  function addMsg(msg) {
    if (!S.conv || S.conv.messages.some((m) => m.id === msg.id)) return false
    S.conv.messages.push(msg)
    return true
  }

  function handle(ev) {
    const here = S.conv && ev.convId === S.conv.id
    switch (ev.type) {
      case 'user_message':
        if (here && addMsg(ev.message)) renderHistory()
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
      case 'reasoning':
      case 'content': {
        const d = S.drafts.get(ev.id)
        if (!d) break
        if (ev.type === 'content') d.content += ev.text
        else d.reasoning += ev.text
        renderLive()
        break
      }
      case 'assistant_done':
        S.drafts.delete(ev.message.id)
        if (here) { addMsg(ev.message); renderHistory() }
        renderLive()
        break
      case 'tool_start':
        S.runningTools.set(ev.callId, ev.label || '')
        if (here) renderHistory(false)
        renderLive()
        break
      case 'tool_done':
        S.runningTools.delete(ev.callId)
        if (here) { addMsg(ev.message); renderHistory(false) }
        if (/^(remember|forget|schedule_message|cancel_schedule)$/.test(ev.message?.name || '')) refreshState()
        renderLive()
        break
      case 'event':
        if (here && addMsg(ev.message)) renderHistory()
        break
      case 'turn_end':
        S.running.delete(ev.convId)
        for (const [id, d] of S.drafts) if (d.convId === ev.convId) S.drafts.delete(id)
        S.runningTools.clear()
        if (here) reloadConv()
        renderLive()
        refreshConvs()
        break
      case 'proactive':
        refreshConvs()
        if (store.get('yz-ding') !== 'off') radio.ding()
        if (here && !document.hidden) markRead()
        else if (!here) toast('另一页有新消息', 'click', 6000, () => loadConv(ev.convId))
        flashTitle()
        break
      case 'compacting':
        if (here) { S.compacting = true; renderLive() }
        break
      case 'compacted':
      case 'compact_failed':
        if (here) { S.compacting = false; reloadConv() }
        break
      case 'stats':
        S.state = { ...S.state, ...ev.state }
        break
      case 'settings':
        S.settings = ev.settings
        S.features = ev.features
        renderHistory(false)
        break
    }
  }
  setInterval(refreshState, 5 * 60 * 1000)

  // ---------------- 发消息 ----------------
  let sizeQueued = false
  function autosize() {
    if (sizeQueued) return
    sizeQueued = true
    requestAnimationFrame(() => {
      sizeQueued = false
      ui.input.style.height = 'auto'
      ui.input.style.height = `${Math.min(ui.input.scrollHeight, window.innerHeight * 0.32)}px`
    })
  }
  ui.input.addEventListener('input', autosize)
  ui.input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing && e.keyCode !== 229) {
      e.preventDefault()
      $('#composer').requestSubmit()
    }
  })

  async function shrinkImage(file) {
    const url = URL.createObjectURL(file)
    try {
      const img = await new Promise((res, rej) => { const i = new Image(); i.onload = () => res(i); i.onerror = rej; i.src = url })
      const k = Math.min(1, 1600 / Math.max(img.width, img.height))
      const c = document.createElement('canvas')
      c.width = Math.round(img.width * k)
      c.height = Math.round(img.height * k)
      const g = c.getContext('2d')
      g.fillStyle = '#fff'
      g.fillRect(0, 0, c.width, c.height)
      g.drawImage(img, 0, 0, c.width, c.height)
      return c.toDataURL('image/jpeg', 0.85)
    } finally { URL.revokeObjectURL(url) }
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
    $('#attachments').innerHTML = S.attachments.map((d, i) => `<div class="att"><img src="${d}" alt=""><button type="button" data-i="${i}" aria-label="移除">×</button></div>`).join('')
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
    if (!S.conv || S.sending) return
    if (S.running.has(S.conv.id)) {
      api('/api/chat/stop', { method: 'POST', body: { convId: S.conv.id } }).catch(() => {})
      return
    }
    const text = ui.input.value.trim()
    const images = S.attachments.slice()
    if (!text && !images.length) return
    S.sending = true
    ui.input.value = ''
    S.attachments = []
    renderAttachments()
    autosize()
    S.running.add(S.conv.id)
    renderLive()
    follow(true)
    try {
      const r = await api('/api/chat', { method: 'POST', body: { convId: S.conv.id, text, images } })
      if (addMsg(r.message)) renderHistory()
    } catch (err) {
      S.running.delete(S.conv.id)
      renderLive()
      ui.input.value = text
      S.attachments = images
      renderAttachments()
      autosize()
      toast(err.message, 'bad', 5000)
    } finally { S.sending = false }
  })

  // ---------------- 弹窗 ----------------
  $$('[data-open]').forEach((b) => b.addEventListener('click', () => { closeDrawer(); openSheet(b.dataset.open) }))
  ui.sheet.addEventListener('click', (e) => {
    if (e.target === ui.sheet || e.target.closest('[data-close]')) ui.sheet.close()
  })
  const closeBtn = `<button class="icon-btn" type="button" data-close aria-label="关闭">${svg(P.x)}</button>`
  function openSheet(kind, tab) {
    if (kind === 'memory') renderMemory(tab)
    if (kind === 'settings') renderSettings(tab)
    if (!ui.sheet.open) ui.sheet.showModal()
  }
  function tabsHtml(tabs, cur) {
    return `<div class="tabs" role="tablist">${tabs.map(([k, v]) => `<button type="button" role="tab" data-tab="${k}" aria-selected="${k === cur}">${v}</button>`).join('')}</div>`
  }

  async function renderMemory(tab = 'notes') {
    await refreshState()
    const memo = S.memory.slice().reverse().map((m) => `
      <div class="item"><div class="grow"><b>${esc(m.text)}</b><span>${fmtTime(m.ts)}</span></div>
      <button class="icon-btn" type="button" data-mem="${esc(m.id)}" aria-label="删除">${svg(P.x)}</button></div>`).join('')
    const sch = S.schedules.slice().sort((a, b) => a.at - b.at).map((s) => `
      <div class="item"><div class="grow"><b>${esc(s.note)}</b><span>${fmtTime(s.at)}${s.repeat === 'daily' ? '，每天' : ''}</span></div>
      <button class="icon-btn" type="button" data-sch="${esc(s.id)}" aria-label="取消">${svg(P.x)}</button></div>`).join('')
    const sum = S.conv?.summary
    ui.sheet.innerHTML = `
      <div class="sheet-head"><h3>回忆</h3>${closeBtn}</div>
      ${tabsHtml([['notes', '长期记忆'], ['plans', '约好的事'], ['diary', '这一页的日记']], tab)}
      <div class="sheet-body">
        <section class="pane" data-pane="notes"${tab === 'notes' ? '' : ' hidden'}>
          <p class="hint">她记下的关于你的事。聊天记录整理之后也不会忘。</p>
          <div class="inline"><input type="text" id="memNew" placeholder="手动添加一条"><button class="btn" type="button" id="memAdd">添加</button></div>
          <div class="list">${memo || '<div class="empty">还没有记下什么</div>'}</div>
        </section>
        <section class="pane" data-pane="plans"${tab === 'plans' ? '' : ' hidden'}>
          <p class="hint">她答应过要来找你的时间，到点会自己来。</p>
          <div class="list">${sch || '<div class="empty">现在没有约好的事</div>'}</div>
        </section>
        <section class="pane" data-pane="diary"${tab === 'diary' ? '' : ' hidden'}>
          ${sum ? `<div class="diary-text">${esc(sum.text)}</div><p class="hint">已整理 ${sum.count} 条，最后一次在 ${fmtTime(sum.ts)}</p>` : '<div class="empty">这一页还没有整理过</div>'}
          <div><button class="btn" type="button" id="compactHere">现在整理</button></div>
        </section>
      </div>`
    $$('.tabs button', ui.sheet).forEach((b) => b.addEventListener('click', () => renderMemory(b.dataset.tab)))
    $('#memAdd', ui.sheet).addEventListener('click', (e) => once(e.currentTarget, async () => {
      const v = $('#memNew', ui.sheet).value.trim()
      if (!v) return
      await api('/api/memory', { method: 'POST', body: { text: v } })
      renderMemory('notes')
    }))
    $('#compactHere', ui.sheet).addEventListener('click', async () => { ui.sheet.close(); await compactNow() })
    $('.sheet-body', ui.sheet).addEventListener('click', async (e) => {
      const m = e.target.closest('[data-mem]')
      const s = e.target.closest('[data-sch]')
      if (m) { await api(`/api/memory/${m.dataset.mem}`, { method: 'DELETE' }); renderMemory('notes') }
      if (s) { await api(`/api/schedules/${s.dataset.sch}`, { method: 'DELETE' }); renderMemory('plans') }
    })
  }

  const CHATTY = { 1: '很少', 2: '偶尔', 3: '时不时', 4: '经常', 5: '非常频繁' }
  const EFFORT = { none: '不思考', low: '简单想想', high: '认真想', max: '深入想' }
  const TIMEZONES = ['Asia/Shanghai', 'Asia/Hong_Kong', 'Asia/Taipei', 'Asia/Tokyo', 'Asia/Seoul', 'Asia/Singapore', 'Europe/London', 'Europe/Berlin', 'America/New_York', 'America/Los_Angeles', 'Australia/Sydney']

  function renderSettings(tab = 'role') {
    const s = S.settings
    const f = S.features
    const p = s.proactive
    const opt = (v, cur, label) => `<option value="${esc(v)}"${v === cur ? ' selected' : ''}>${esc(label)}</option>`
    const radioOpt = (name, v, cur, label) => `<label><input type="radio" name="${name}" value="${v}"${v === cur ? ' checked' : ''}>${label}</label>`
    ui.sheet.innerHTML = `
      <div class="sheet-head"><h3>设置</h3>${closeBtn}</div>
      ${tabsHtml([['role', '角色'], ['master', '最高指令'], ['chat', '主动聊天'], ['model', '模型'], ['sound', '声音'], ['account', '账号']], tab)}
      <form class="sheet-body" id="setForm" autocomplete="off">
        <section class="pane" data-pane="role">
          <div class="two">
            <div class="field"><label for="sName">名字</label><input type="text" id="sName" value="${esc(s.charName)}" maxlength="20"></div>
            <div class="field"><label for="sUser">怎么称呼你</label><input type="text" id="sUser" value="${esc(s.userName)}" maxlength="20"></div>
          </div>
          <div class="field"><label for="sPersona">人设</label>
            <p class="hint">性格、说话方式、你们的关系。写得越具体越像。</p>
            <textarea id="sPersona" class="tall">${esc(s.persona)}</textarea></div>
          <div class="field"><label for="sTz">时区</label>
            <input type="text" id="sTz" list="tzList" value="${esc(s.timezone)}"><datalist id="tzList">${TIMEZONES.map((z) => `<option value="${z}">`).join('')}</datalist></div>
        </section>

        <section class="pane" data-pane="master">
          <div class="field"><label for="sMaster">最高指令</label>
            <p class="hint">优先级高于人设和网站内置的所有规则，冲突时以这里为准。</p>
            <textarea id="sMaster" class="tall">${esc(s.masterPrompt || '')}</textarea></div>
          <div class="field"><span class="flabel">使用方式</span>
            <div class="choice">${radioOpt('mMode', 'top', s.masterMode || 'top', '放在最前面，其它规则照常')}${radioOpt('mMode', 'replace', s.masterMode || 'top', '完全替换，只用这里的')}</div>
            <p class="hint">完全替换后，人设和内置规则都不再生效，只保留这段内容、长期记忆和工具。</p></div>
        </section>

        <section class="pane" data-pane="chat">
          <label class="switch"><input type="checkbox" id="sPro"${p.enabled ? ' checked' : ''}>允许她主动来找我</label>
          <div class="field"><span class="flabel">频率 <span class="range-now" id="chatNow">${CHATTY[Math.round(p.chattiness)]}</span></span>
            <input type="range" id="sChat" min="1" max="5" step="1" value="${p.chattiness}">
            <div class="range-labels"><span>很少</span><span>非常频繁</span></div>
            <p class="hint">什么时候来是随机的。你不回，她会慢慢少来；你一回，就恢复。</p></div>
          <div class="two">
            <div class="field"><label for="sSleep">大约几点睡</label><input type="time" id="sSleep" value="${esc(p.sleepStart)}"></div>
            <div class="field"><label for="sWake">大约几点起</label><input type="time" id="sWake" value="${esc(p.sleepEnd)}"></div>
          </div>
          <p class="hint">每天会前后浮动半小时左右。睡着时不会主动来，约好的提醒除外。</p>
          <div><button class="btn" type="button" id="pokeNow">现在试一次</button></div>
        </section>

        <section class="pane" data-pane="model">
          <div class="two">
            <div class="field"><label for="sModel">模型</label>
              <select id="sModel">${f.models.map((m) => opt(m, s.model, m === 'deepseek-flash' ? 'deepseek-flash（快，能看图）' : m === 'deepseek-v4-pro' ? 'deepseek-v4-pro（更强，不能看图）' : m)).join('')}</select></div>
            <div class="field"><label for="sEffort">思考程度</label>
              <select id="sEffort">${f.efforts.map((x) => opt(x, s.effort, EFFORT[x] || x)).join('')}</select></div>
          </div>
          <div class="field"><span class="flabel">看图片和截图</span>
            <div class="choice">${radioOpt('vis', 'auto', s.vision, '自动')}${radioOpt('vis', 'on', s.vision, '总是')}${radioOpt('vis', 'off', s.vision, '不看')}</div></div>
          <div class="two">
            <div class="field"><label for="sCompact">聊到多长自动整理</label><input type="number" id="sCompact" min="20000" max="800000" step="10000" value="${s.compactAt}"><p class="hint">单位 token，一个汉字约 0.6</p></div>
            <div class="field"><label for="sKeep">整理时保留最近</label><input type="number" id="sKeep" min="4000" max="200000" step="2000" value="${s.keepRecent}"></div>
          </div>
          <div class="notice">可用工具：${Object.values(f.tools).map((t) => esc(t.label)).join('、')}。工作目录 <code>${esc(f.workdir)}</code>，操作不需要确认。${f.browser ? '' : '浏览器组件未安装。'}</div>
        </section>

        <section class="pane" data-pane="sound">
          <div class="field"><label for="sVol">音量</label><input type="range" id="sVol" min="0" max="1" step="0.05" value="${radio.volume}"></div>
          <label class="switch"><input type="checkbox" id="sDing"${store.get('yz-ding') !== 'off' ? ' checked' : ''}>收到主动消息时提示音</label>
          <div class="field"><span class="flabel">歌曲</span>
            <p class="hint">没有上传时，播放现场生成的轻音乐。支持 mp3、m4a、ogg、flac、wav。</p>
            <div class="list" id="songList"></div>
            <div class="inline"><label class="btn">上传<input type="file" id="songFile" accept="audio/*" multiple hidden></label><button class="btn" type="button" id="songNext">下一首</button></div></div>
        </section>

        <section class="pane" data-pane="account">
          <div class="field"><label for="sKey">DeepSeek 密钥</label>
            <p class="hint">${f.apiKey ? (f.apiKeyFromEnv ? '已在服务器 .env 中设置，这里填写会覆盖它。' : '已设置。留空则不修改。') : '尚未设置，可在 platform.deepseek.com 申请。'}</p>
            <input type="password" id="sKey" placeholder="${f.apiKey ? '留空不修改' : 'sk-'}" autocomplete="off"></div>
          <div class="field"><label for="sPass">修改暗号</label><input type="password" id="sPass" placeholder="留空不修改，至少 6 位" autocomplete="new-password"></div>
          <div class="field"><span class="flabel">手机通知</span>
            <p class="hint">${window.isSecureContext ? '开启后，网页关着也能收到消息提醒。' : '需要用 https 地址打开网站才能开启。'}</p>
            <div class="inline"><button class="btn" type="button" id="pushOn"${window.isSecureContext && f.push ? '' : ' disabled'}>开启通知</button><button class="btn" type="button" id="pushTest"${window.isSecureContext && f.push ? '' : ' disabled'}>发一条测试</button></div></div>
          <div><button class="btn btn-danger" type="button" id="logout">退出登录</button></div>
        </section>
      </form>
      <div class="sheet-foot"><button class="btn" type="button" data-close>取消</button><button class="btn btn-primary" type="submit" form="setForm">保存</button></div>`

    const showTab = (k) => {
      $$('.tabs button', ui.sheet).forEach((b) => b.setAttribute('aria-selected', String(b.dataset.tab === k)))
      $$('.pane', ui.sheet).forEach((el) => { el.hidden = el.dataset.pane !== k })
      if (k === 'sound') renderSongs()
    }
    $$('.tabs button', ui.sheet).forEach((b) => b.addEventListener('click', () => showTab(b.dataset.tab)))
    showTab(tab)

    $('#sChat', ui.sheet).addEventListener('input', (e) => { $('#chatNow', ui.sheet).textContent = CHATTY[e.target.value] })
    $('#sVol', ui.sheet).addEventListener('input', (e) => radio.setVolume(+e.target.value))
    $('#sDing', ui.sheet).addEventListener('change', (e) => { store.set('yz-ding', e.target.checked ? 'on' : 'off'); if (e.target.checked) radio.ding() })
    $('#songNext', ui.sheet).addEventListener('click', () => radio.next())
    $('#pokeNow', ui.sheet).addEventListener('click', (e) => once(e.currentTarget, async () => {
      try {
        await api('/api/proactive/test', { method: 'POST', body: { kind: 'idle' } })
        ui.sheet.close()
        toast('已经叫她了，稍等')
      } catch (err) { toast(err.message, 'bad') }
    }))
    $('#songFile', ui.sheet).addEventListener('change', async (e) => {
      for (const file of e.target.files) {
        toast(`正在上传 ${file.name}`)
        try {
          await api('/api/music', { method: 'POST', raw: true, body: file, headers: { 'X-Filename': encodeURIComponent(file.name), 'Content-Type': 'application/octet-stream' } })
        } catch (err) { toast(err.message, 'bad') }
      }
      e.target.value = ''
      await loadMusic()
      renderSongs()
    })
    $('#pushOn', ui.sheet).addEventListener('click', enablePush)
    $('#pushTest', ui.sheet).addEventListener('click', () => api('/api/push/test', { method: 'POST', body: {} }).then(() => toast('已发送')).catch((e) => toast(e.message, 'bad')))
    $('#logout', ui.sheet).addEventListener('click', async () => {
      await api('/api/logout', { method: 'POST', body: {} })
      ui.sheet.close()
      radio.pause()
      showLogin()
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
      await once($('.sheet-foot .btn-primary', ui.sheet), async () => {
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
          renderHistory(false)
          ui.sheet.close()
          toast('已保存')
        } catch (err) { toast(err.message, 'bad', 5000) }
      })
    })
  }

  function renderSongs() {
    const box = $('#songList', ui.sheet)
    if (!box) return
    box.innerHTML = radio.tracks.length ? radio.tracks.map((n) => `
      <div class="item"><div class="grow"><b>${esc(n.replace(/\.[^.]+$/, ''))}</b></div>
      <button class="icon-btn" type="button" data-song="${esc(n)}" aria-label="删除">${svg(P.trash)}</button></div>`).join('') : '<div class="empty">还没有上传</div>'
    box.onclick = async (e) => {
      const b = e.target.closest('[data-song]')
      if (!b || !confirm('删除这首歌？')) return
      await api(`/api/music/${encodeURIComponent(b.dataset.song)}`, { method: 'DELETE' })
      await loadMusic()
      renderSongs()
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
      if (!('serviceWorker' in navigator) || !('PushManager' in window)) throw new Error('这个浏览器不支持通知。iPhone 和 iPad 需要先"添加到主屏幕"，再从桌面图标打开')
      const perm = await Notification.requestPermission()
      if (perm !== 'granted') throw new Error('没有允许通知')
      const reg = await navigator.serviceWorker.register('/sw.js')
      await navigator.serviceWorker.ready
      const { key } = await api('/api/push/key')
      let sub = await reg.pushManager.getSubscription()
      if (!sub) sub = await reg.pushManager.subscribe({ userVisibleOnly: true, applicationServerKey: b64ToU8(key) })
      await api('/api/push/subscribe', { method: 'POST', body: sub.toJSON() })
      toast('通知已开启')
    } catch (err) { toast(err.message, 'bad', 6000) }
  }
  if ('serviceWorker' in navigator && window.isSecureContext) navigator.serviceWorker.register('/sw.js').catch(() => {})

  // ---------------- 启动 ----------------
  ;(async () => {
    try {
      const me = await api('/api/me')
      if (me.authed) await enter()
      else showLogin()
    } catch { showLogin() }
  })()
})()
