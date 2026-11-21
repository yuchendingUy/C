import { loadEnv, ROOT } from './util.js'
loadEnv()

const http = await import('node:http')
const fs = await import('node:fs')
const fsp = await import('node:fs/promises')
const path = await import('node:path')
const crypto = await import('node:crypto')

const store = await import('./store.js')
const { runTurn, isRunning, runningIds, stopTurn, compactConversation, visionOn } = await import('./agent.js')
const { addClient, broadcast } = await import('./bus.js')
const { hasKey, MODELS, EFFORTS } = await import('./deepseek.js')
const { browserAvailable, toolMeta } = await import('./tools/index.js')
const { MOODS } = await import('./tools/life.js')
const push = await import('./push.js')
const { startProactive, noteUserActivity, fire, isAsleep } = await import('./proactive.js')
const leisure = await import('./leisure.js')

const PORT = Number(process.env.PORT || 8787)
const HOST = process.env.HOST || '0.0.0.0'
const PUBLIC = path.join(ROOT, 'public')
const COOKIE = 'yz_s'

const STATIC_ROOTS = {
  '/fonts/kuaile/': path.join(ROOT, 'node_modules/@fontsource/zcool-kuaile/'),
  '/fonts/wenkai/': path.join(ROOT, 'node_modules/lxgw-wenkai-screen-webfont/'),
}

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg',
  '.webp': 'image/webp', '.gif': 'image/gif', '.ico': 'image/x-icon', '.woff2': 'font/woff2', '.woff': 'font/woff',
  '.mp3': 'audio/mpeg', '.ogg': 'audio/ogg', '.opus': 'audio/ogg', '.m4a': 'audio/mp4', '.aac': 'audio/aac',
  '.wav': 'audio/wav', '.flac': 'audio/flac', '.webmanifest': 'application/manifest+json',
}
const AUDIO_EXT = new Set(['.mp3', '.ogg', '.opus', '.m4a', '.aac', '.wav', '.flac'])

// ---------------- 小工具 ----------------
const send = (res, code, data, headers = {}) => {
  const body = typeof data === 'string' ? data : JSON.stringify(data)
  res.writeHead(code, { 'Content-Type': typeof data === 'string' ? 'text/plain; charset=utf-8' : 'application/json; charset=utf-8', 'Cache-Control': 'no-store', ...headers })
  res.end(body)
}
const fail = (res, code, message) => send(res, code, { error: message })

function readBody(req, limit = 25 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    const chunks = []
    let size = 0
    req.on('data', (c) => {
      size += c.length
      if (size > limit) {
        reject(Object.assign(new Error('内容太大了'), { status: 413 }))
        req.destroy()
      } else chunks.push(c)
    })
    req.on('end', () => resolve(Buffer.concat(chunks)))
    req.on('error', reject)
  })
}
const readJSONBody = async (req) => {
  const buf = await readBody(req)
  if (!buf.length) return {}
  try { return JSON.parse(buf.toString('utf8')) } catch { throw Object.assign(new Error('JSON 格式不对'), { status: 400 }) }
}

const cookies = (req) => Object.fromEntries((req.headers.cookie || '').split(';').map((c) => c.trim().split('=')).filter((x) => x[0]).map(([k, ...v]) => [k, decodeURIComponent(v.join('='))]))
const authed = (req) => store.checkSession(cookies(req)[COOKIE])
const cookieStr = (val, maxAge) => `${COOKIE}=${val}; Path=/; HttpOnly; SameSite=Lax; Max-Age=${maxAge}${process.env.COOKIE_SECURE === 'true' ? '; Secure' : ''}`

function serveFile(req, res, file, { cache = 'no-cache' } = {}) {
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) return fail(res, 404, '找不到')
    const type = MIME[path.extname(file).toLowerCase()] || 'application/octet-stream'
    const range = req.headers.range && /bytes=(\d*)-(\d*)/.exec(req.headers.range)
    if (range) {
      const start = range[1] ? Number(range[1]) : 0
      const end = range[2] ? Math.min(Number(range[2]), st.size - 1) : st.size - 1
      if (start > end || start >= st.size) { res.writeHead(416, { 'Content-Range': `bytes */${st.size}` }); return res.end() }
      res.writeHead(206, { 'Content-Type': type, 'Content-Range': `bytes ${start}-${end}/${st.size}`, 'Accept-Ranges': 'bytes', 'Content-Length': end - start + 1, 'Cache-Control': cache })
      return fs.createReadStream(file, { start, end }).pipe(res)
    }
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': st.size, 'Accept-Ranges': 'bytes', 'Cache-Control': cache })
    fs.createReadStream(file).pipe(res)
  })
}

// 防止路径穿越
const inside = (base, rel) => {
  const p = path.resolve(base, '.' + path.sep + rel)
  return p.startsWith(path.resolve(base)) ? p : null
}

// ---------------- 暗号（登录） ----------------
const fails = new Map()
function passOk(input) {
  input = String(input || '')
  const hash = store.getSecret('passHash')
  if (hash) {
    const [salt, key] = hash.split(':')
    const got = crypto.scryptSync(input, salt, 32)
    return crypto.timingSafeEqual(got, Buffer.from(key, 'hex'))
  }
  const env = process.env.SITE_PASSWORD || ''
  if (!env) return false
  const a = crypto.createHash('sha256').update(input).digest()
  const b = crypto.createHash('sha256').update(env).digest()
  return crypto.timingSafeEqual(a, b)
}
async function setPassword(pw) {
  const salt = crypto.randomBytes(16).toString('hex')
  const key = crypto.scryptSync(String(pw), salt, 32).toString('hex')
  await store.setSecret('passHash', `${salt}:${key}`)
}

// ---------------- 给前端的数据 ----------------
function publicSettings() {
  const s = store.getSettings()
  return s
}
function features() {
  const s = store.getSettings()
  return {
    apiKey: hasKey(),
    apiKeyFromEnv: !!process.env.DEEPSEEK_API_KEY && !store.getSecret('deepseekKey'),
    browser: browserAvailable,
    vision: visionOn(s),
    push: push.pushAvailable(),
    models: MODELS,
    efforts: EFFORTS,
    moods: MOODS,
    tools: toolMeta(),
    workdir: process.env.WORKDIR || process.env.HOME || '/root',
  }
}

const clampNum = (v, lo, hi, d) => {
  const n = Number(v)
  return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : d
}
const hhmm = (v, d) => (/^\d{1,2}:\d{2}$/.test(String(v || '')) ? String(v).padStart(5, '0') : d)

function cleanSettings(input, cur) {
  const s = structuredClone(cur)
  const str = (v, max, d) => (typeof v === 'string' ? v.slice(0, max) : d)
  s.charName = str(input.charName, 20, s.charName).trim() || '柚柚'
  s.userName = str(input.userName, 20, s.userName).trim() || '你'
  s.persona = str(input.persona, 8000, s.persona)
  s.masterPrompt = str(input.masterPrompt, 20000, s.masterPrompt || '')
  s.masterMode = input.masterMode === 'replace' ? 'replace' : 'top'
  if (input.timezone) {
    try { new Intl.DateTimeFormat('zh-CN', { timeZone: input.timezone }); s.timezone = input.timezone } catch {}
  }
  if (MODELS.includes(input.model)) s.model = input.model
  if (EFFORTS.includes(input.effort)) s.effort = input.effort
  if (['auto', 'on', 'off'].includes(input.vision)) s.vision = input.vision
  const p = input.proactive || {}
  s.proactive = {
    enabled: p.enabled !== undefined ? !!p.enabled : s.proactive.enabled,
    chattiness: clampNum(p.chattiness, 1, 5, s.proactive.chattiness),
    sleepStart: hhmm(p.sleepStart, s.proactive.sleepStart),
    sleepEnd: hhmm(p.sleepEnd, s.proactive.sleepEnd),
  }
  if (input.leisure) {
    const L = input.leisure
    const acts = Array.isArray(L.activities) ? L.activities : String(L.activities || '').split('\n')
    s.leisure = {
      enabled: L.enabled !== undefined ? !!L.enabled : s.leisure.enabled,
      everyMin: clampNum(L.everyMin, 2, 240, s.leisure.everyMin),
      activities: acts.map((x) => String(x).trim().slice(0, 200)).filter(Boolean).slice(0, 40),
    }
  }
  s.compactAt = clampNum(input.compactAt, 20000, 800000, s.compactAt)
  s.keepRecent = clampNum(input.keepRecent, 4000, 200000, s.keepRecent)
  return s
}

// ---------------- 路由 ----------------
const routes = []
const route = (method, pattern, handler, { open = false } = {}) => {
  const keys = []
  const re = new RegExp(`^${pattern.replace(/:(\w+)/g, (_, k) => (keys.push(k), '([^/]+)'))}$`)
  routes.push({ method, re, keys, handler, open })
}

route('POST', '/api/login', async (req, res) => {
  const ip = req.socket.remoteAddress
  const f = fails.get(ip) || { n: 0, until: 0 }
  if (f.until > Date.now()) return fail(res, 429, `试错太多次了，${Math.ceil((f.until - Date.now()) / 60000)} 分钟后再来`)
  const { password } = await readJSONBody(req)
  if (!store.getSecret('passHash') && !process.env.SITE_PASSWORD) return fail(res, 503, '还没设置暗号：在 .env 里填 SITE_PASSWORD 然后重启')
  if (!passOk(password)) {
    f.n++
    if (f.n >= 6) { f.until = Date.now() + 15 * 60 * 1000; f.n = 0 }
    fails.set(ip, f)
    return fail(res, 401, '暗号不对')
  }
  fails.delete(ip)
  const token = await store.createSession()
  send(res, 200, { ok: true }, { 'Set-Cookie': cookieStr(token, 30 * 86400) })
}, { open: true })

route('POST', '/api/logout', async (req, res) => {
  await store.dropSession(cookies(req)[COOKIE])
  send(res, 200, { ok: true }, { 'Set-Cookie': cookieStr('', 0) })
}, { open: true })

route('GET', '/api/me', (req, res) => send(res, 200, { authed: authed(req) }), { open: true })

route('GET', '/api/state', async (req, res) => {
  const main = await store.ensureMain()
  send(res, 200, {
    settings: publicSettings(),
    state: { ...store.getState(), asleep: isAsleep() },
    mainId: main.id,
    conversations: store.listConversations(),
    running: runningIds(),
    leisure: leisure.leisureStatus(),
    features: features(),
    memory: store.getMemory(),
    schedules: store.getSchedules(),
  })
})

route('GET', '/api/events', (req, res) => addClient(res))

route('GET', '/api/conversations', (req, res) => send(res, 200, store.listConversations()))
route('POST', '/api/conversations', async (req, res) => {
  const { title } = await readJSONBody(req)
  const c = await store.createConversation(String(title || '新的一页').slice(0, 40))
  await store.patchState({ mainId: c.id })
  send(res, 200, c)
})
route('GET', '/api/conversations/:id', (req, res, p) => {
  const c = store.getConversation(p.id)
  c ? send(res, 200, c) : fail(res, 404, '没有这一页')
})
route('PATCH', '/api/conversations/:id', async (req, res, p) => {
  const c = store.getConversation(p.id)
  if (!c) return fail(res, 404, '没有这一页')
  const { title } = await readJSONBody(req)
  if (title) c.title = String(title).slice(0, 40)
  await store.saveConversation(c)
  send(res, 200, { ok: true })
})
route('DELETE', '/api/conversations/:id', async (req, res, p) => {
  if (isRunning(p.id)) return fail(res, 409, '正在聊着呢，先停下来')
  await store.deleteConversation(p.id)
  if (store.getState().mainId === p.id) await store.patchState({ mainId: null })
  send(res, 200, { ok: true })
})
route('POST', '/api/conversations/:id/read', async (req, res, p) => {
  const c = store.getConversation(p.id)
  if (!c) return fail(res, 404, '没有这一页')
  if (c.unread) { c.unread = 0; await store.saveConversation(c) }
  await store.patchState({ mainId: c.id })
  send(res, 200, { ok: true })
})
route('POST', '/api/conversations/:id/compact', async (req, res, p) => {
  const c = store.getConversation(p.id)
  if (!c) return fail(res, 404, '没有这一页')
  if (isRunning(c.id)) return fail(res, 409, '她正在说话，等一下再整理')
  if (!hasKey()) return fail(res, 400, '还没有填 DeepSeek 密钥')
  broadcast({ type: 'compacting', convId: c.id })
  try {
    const r = await compactConversation(c, { manual: true, emit: broadcast })
    send(res, 200, r)
  } catch (e) {
    broadcast({ type: 'compact_failed', convId: c.id })
    throw e
  }
})

route('POST', '/api/chat', async (req, res) => {
  const { convId, text, images } = await readJSONBody(req)
  const c = store.getConversation(convId)
  if (!c) return fail(res, 404, '没有这一页')
  if (!hasKey()) return fail(res, 400, '还没有填 DeepSeek 密钥，去「小本本」里填一下')
  if (isRunning(c.id)) return fail(res, 409, '还在回上一句，等说完或者先停下')
  const content = String(text || '').slice(0, 50000)
  const fileIds = []
  for (const img of (Array.isArray(images) ? images : []).slice(0, 6)) {
    const m = /^data:image\/(png|jpe?g|webp|gif);base64,(.+)$/.exec(String(img))
    if (m) fileIds.push(await store.saveFile(Buffer.from(m[2], 'base64'), m[1] === 'jpeg' ? 'jpg' : m[1]))
  }
  if (!content.trim() && !fileIds.length) return fail(res, 400, '说点什么吧')
  const msg = store.addMessage(c, { role: 'user', content, ...(fileIds.length ? { images: fileIds } : {}) })
  if (c.messages.filter((m) => m.role === 'user' && !m.hidden).length === 1 && /^(新的一页|第一页)$/.test(c.title)) {
    c.title = content.replace(/\s+/g, ' ').slice(0, 18) || '图片'
  }
  c.unread = 0
  await store.saveConversation(c)
  await store.patchState({ mainId: c.id })
  await noteUserActivity()
  broadcast({ type: 'user_message', convId: c.id, message: msg })
  send(res, 202, { ok: true, message: msg })
  runTurn(c).catch((e) => console.error('[chat]', e))
})

route('POST', '/api/chat/stop', async (req, res) => {
  const { convId } = await readJSONBody(req)
  stopTurn(convId)
  send(res, 200, { ok: true })
})

route('PUT', '/api/settings', async (req, res) => {
  const input = await readJSONBody(req)
  const s = await store.saveSettings(cleanSettings(input, store.getSettings()))
  broadcast({ type: 'settings', settings: s, features: features() })
  send(res, 200, { settings: s, features: features() })
})

route('PUT', '/api/secrets', async (req, res) => {
  const { deepseekKey, newPassword } = await readJSONBody(req)
  if (deepseekKey !== undefined) await store.setSecret('deepseekKey', String(deepseekKey).trim())
  if (newPassword !== undefined) {
    if (String(newPassword).length < 6) return fail(res, 400, '新暗号至少 6 位')
    await setPassword(newPassword)
  }
  send(res, 200, { ok: true, features: features() })
})

route('POST', '/api/pet', async (req, res) => {
  const st = store.getState()
  const s = store.getSettings()
  const lp = (await import('./util.js')).localParts(new Date(), s.timezone)
  const count = st.petDay.date === lp.date ? st.petDay.count : 0
  const patch = { petDay: { date: lp.date, count: count + 1 } }
  if (count < 3) patch.affection = Math.min(100, st.affection + 1)
  await store.patchState(patch)
  broadcast({ type: 'stats', state: store.getState() })
  send(res, 200, { state: store.getState() })
})

route('POST', '/api/proactive/test', async (req, res) => {
  const { kind } = await readJSONBody(req)
  send(res, 202, { ok: true })
  fire(kind === 'wake' ? 'wake' : 'idle').catch((e) => console.error('[proactive test]', e))
})

route('DELETE', '/api/memory/:id', async (req, res, p) => {
  await store.removeMemory(p.id)
  send(res, 200, { ok: true, memory: store.getMemory() })
})
route('POST', '/api/memory', async (req, res) => {
  const { text } = await readJSONBody(req)
  if (!String(text || '').trim()) return fail(res, 400, '写点什么')
  await store.addMemory(String(text).trim())
  send(res, 200, { ok: true, memory: store.getMemory() })
})
route('DELETE', '/api/schedules/:id', async (req, res, p) => {
  await store.updateSchedules((l) => l.filter((s) => s.id !== p.id))
  send(res, 200, { ok: true, schedules: store.getSchedules() })
})

route('GET', '/api/files/:id', (req, res, p) => serveFile(req, res, store.filePath(p.id), { cache: 'private, max-age=31536000' }))

// 音乐
route('GET', '/api/music', async (req, res) => {
  const files = (await fsp.readdir(store.DIRS.music)).filter((f) => AUDIO_EXT.has(path.extname(f).toLowerCase())).sort()
  send(res, 200, files)
})
route('POST', '/api/music', async (req, res) => {
  const raw = decodeURIComponent(String(req.headers['x-filename'] || 'song.mp3'))
  const name = path.basename(raw).replace(/[\\/:*?"<>|\x00-\x1f]/g, '_').slice(0, 120)
  if (!AUDIO_EXT.has(path.extname(name).toLowerCase())) return fail(res, 400, '只收 mp3 / ogg / m4a / wav / flac / opus')
  const buf = await readBody(req, 60 * 1024 * 1024)
  await fsp.writeFile(path.join(store.DIRS.music, name), buf)
  send(res, 200, { ok: true, name })
})
route('DELETE', '/api/music/:name', async (req, res, p) => {
  const f = inside(store.DIRS.music, decodeURIComponent(p.name))
  if (f) await fsp.rm(f, { force: true })
  send(res, 200, { ok: true })
})
route('GET', '/music/:name', (req, res, p) => {
  const f = inside(store.DIRS.music, decodeURIComponent(p.name))
  f ? serveFile(req, res, f, { cache: 'private, max-age=86400' }) : fail(res, 404, '找不到')
})

// 朋友圈
route('GET', '/api/moments', (req, res) => {
  const url = new URL(req.url, 'http://x')
  const before = Number(url.searchParams.get('before')) || Infinity
  const all = store.getMoments().filter((m) => m.ts < before)
  const items = all.slice(-30).reverse()
  send(res, 200, { items, more: all.length > 30, leisure: leisure.leisureStatus() })
})
route('POST', '/api/moments/:id/like', async (req, res, p) => {
  const m = await store.updateMoment(p.id, (x) => { x.liked = !x.liked })
  if (!m) return fail(res, 404, '没有这条')
  broadcast({ type: 'moment_update', moment: m })
  send(res, 200, m)
})
route('POST', '/api/moments/:id/comments', async (req, res, p) => {
  const { text } = await readJSONBody(req)
  const t = String(text || '').trim().slice(0, 500)
  if (!t) return fail(res, 400, '写点什么')
  const m = await store.updateMoment(p.id, (x) => x.comments.push({ id: Math.random().toString(36).slice(2, 8), who: 'me', text: t, ts: Date.now() }))
  if (!m) return fail(res, 404, '没有这条')
  broadcast({ type: 'moment_update', moment: m })
  send(res, 200, m)
  leisure.replyToComment(p.id)
})
route('DELETE', '/api/moments/:id', async (req, res, p) => {
  await store.removeMoment(p.id)
  broadcast({ type: 'moment_removed', id: p.id })
  send(res, 200, { ok: true })
})
route('GET', '/api/leisure/log', (req, res) => send(res, 200, { log: store.getLeisureLog().slice(-60).reverse(), current: leisure.leisureStatus() }))
route('POST', '/api/leisure/now', async (req, res) => {
  const { activity } = await readJSONBody(req)
  if (leisure.leisureStatus()) return fail(res, 409, '她已经在逛了')
  if (!hasKey()) return fail(res, 400, '还没有填 DeepSeek 密钥')
  send(res, 202, { ok: true })
  leisure.goOut(activity ? String(activity).slice(0, 200) : undefined).catch((e) => console.error('[leisure now]', e))
})

route('GET', '/api/push/key', (req, res) => send(res, 200, { key: push.publicKey(), available: push.pushAvailable() }))
route('POST', '/api/push/subscribe', async (req, res) => {
  await push.subscribe(await readJSONBody(req))
  send(res, 200, { ok: true })
})
route('POST', '/api/push/unsubscribe', async (req, res) => {
  const { endpoint } = await readJSONBody(req)
  await push.unsubscribe(endpoint)
  send(res, 200, { ok: true })
})
route('POST', '/api/push/test', async (req, res) => {
  const s = store.getSettings()
  await push.notifyAll({ title: '小屋', body: '通知可以收到了', tag: 'yuzu-test' })
  send(res, 200, { ok: true })
})

// ---------------- 服务器 ----------------
const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x')
  const pathname = decodeURIComponent(url.pathname)
  try {
    for (const r of routes) {
      if (r.method !== req.method) continue
      const m = r.re.exec(url.pathname)
      if (!m) continue
      if (!r.open && !authed(req)) return fail(res, 401, '先对暗号')
      const params = Object.fromEntries(r.keys.map((k, i) => [k, m[i + 1]]))
      return await r.handler(req, res, params)
    }
    if (pathname.startsWith('/api/')) return fail(res, 404, '没有这个接口')
    if (req.method !== 'GET' && req.method !== 'HEAD') return fail(res, 405, '不支持')

    for (const [prefix, base] of Object.entries(STATIC_ROOTS)) {
      if (pathname.startsWith(prefix)) {
        const f = inside(base, pathname.slice(prefix.length))
        return f ? serveFile(req, res, f, { cache: 'public, max-age=31536000, immutable' }) : fail(res, 404, '找不到')
      }
    }
    const f = inside(PUBLIC, pathname === '/' ? 'index.html' : pathname.slice(1))
    if (f && fs.existsSync(f) && fs.statSync(f).isFile()) return serveFile(req, res, f)
    return serveFile(req, res, path.join(PUBLIC, 'index.html'))
  } catch (e) {
    if (!res.headersSent) fail(res, e.status || 500, e.message || '出错了')
    if (!e.status) console.error('[http]', e)
  }
})

await push.initPush()
server.listen(PORT, HOST, () => {
  console.log(`网站已启动：http://${HOST === '0.0.0.0' ? '服务器IP' : HOST}:${PORT}`)
  if (!process.env.SITE_PASSWORD && !store.getSecret('passHash')) console.warn('⚠ 还没设置暗号（.env 里的 SITE_PASSWORD），现在谁都进不来')
  if (!hasKey()) console.warn('⚠ 还没有 DeepSeek 密钥，进网站后在「小本本」里填')
})
startProactive()
leisure.startLeisure()

const shutdown = () => {
  server.close()
  process.exit(0)
}
process.on('SIGTERM', shutdown)
process.on('SIGINT', shutdown)
