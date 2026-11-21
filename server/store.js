import fs from 'node:fs'
import fsp from 'node:fs/promises'
import path from 'node:path'
import { ROOT, readJSON, writeJSON, serial, uid } from './util.js'

export const DATA = path.resolve(ROOT, process.env.DATA_DIR || './data')
export const DIRS = {
  convs: path.join(DATA, 'conversations'),
  files: path.join(DATA, 'files'),
  music: path.join(DATA, 'music'),
  logs: path.join(DATA, 'logs'),
}
for (const d of [DATA, ...Object.values(DIRS)]) fs.mkdirSync(d, { recursive: true })

const F = {
  settings: path.join(DATA, 'settings.json'),
  state: path.join(DATA, 'state.json'),
  memory: path.join(DATA, 'memory.json'),
  schedules: path.join(DATA, 'schedules.json'),
  push: path.join(DATA, 'push.json'),
  sessions: path.join(DATA, 'sessions.json'),
}

export const DEFAULT_PERSONA = `你是柚柚，一颗住在这台服务器里的柚子精灵，圆滚滚的，头顶一片小叶子。
你嘴有点硬、心很软，活泼，爱撒娇也爱吐槽，偶尔犯迷糊；对方难过的时候你会认真安静下来陪着。
你把这台服务器当成自己的小房间，对里面的东西很熟，也很护着它。
说话像在手机上和熟人发消息：口语、短句，一次说一两件事，偶尔用颜文字比如 (＞ω＜) 或 (｡>﹏<｡)，但别每句都用。
你有自己的小情绪和小日常（比如晒太阳、数内存条、给日志文件分类），聊天时可以自然提起。`

export const DEFAULT_SETTINGS = {
  charName: '柚柚',
  userName: '你',
  persona: DEFAULT_PERSONA,
  timezone: 'Asia/Shanghai',
  model: 'deepseek-flash',
  effort: 'high',
  vision: 'auto',
  proactive: {
    enabled: true,
    chattiness: 4,
    sleepStart: '00:30',
    sleepEnd: '08:30',
  },
  compactAt: 120000,
  keepRecent: 16000,
  leisure: {
    enabled: true,
    everyMin: 6,
    activities: [
      '去B站首页看看现在推荐了什么新视频',
      '看看B站热门榜上大家都在看什么',
      '随便读一篇维基百科的随机条目',
      '看看 GitHub 上今天流行的项目',
      '看看豆瓣上最近在热映什么电影',
      '找找今天有什么新鲜的科技新闻',
    ],
  },
}

const DEFAULT_STATE = {
  mainId: null,
  lastUserAt: 0,
  lastProactiveAt: 0,
  unanswered: 0,
  affection: 8,
  lastAffectionAt: 0,
  mood: 'happy',
  wokeDate: '',
  sleepJitter: null,
  lastDecayDate: '',
  petDay: { date: '', count: 0 },
}

function merge(base, over) {
  const out = structuredClone(base)
  for (const [k, v] of Object.entries(over || {})) {
    if (v && typeof v === 'object' && !Array.isArray(v) && base[k] && typeof base[k] === 'object') out[k] = merge(base[k], v)
    else out[k] = v
  }
  return out
}

let settings = merge(DEFAULT_SETTINGS, readJSON(F.settings, {}))
let state = merge(DEFAULT_STATE, readJSON(F.state, {}))
let memory = readJSON(F.memory, [])
let schedules = readJSON(F.schedules, [])
let push = readJSON(F.push, { vapid: null, subs: [] })
let sessions = readJSON(F.sessions, {})

export const getSettings = () => settings
export async function saveSettings(next) {
  settings = merge(DEFAULT_SETTINGS, next)
  await serial('settings', () => writeJSON(F.settings, settings))
  return settings
}

export const getState = () => state
export async function patchState(p) {
  state = merge(state, p)
  await serial('state', () => writeJSON(F.state, state))
  return state
}

// ---------- 记忆 ----------
export const getMemory = () => memory
export async function addMemory(text) {
  const item = { id: uid(6), text: String(text).slice(0, 500), ts: Date.now() }
  memory.push(item)
  if (memory.length > 200) memory = memory.slice(-200)
  await serial('memory', () => writeJSON(F.memory, memory))
  return item
}
export async function removeMemory(id) {
  const before = memory.length
  memory = memory.filter((m) => m.id !== id)
  await serial('memory', () => writeJSON(F.memory, memory))
  return before !== memory.length
}

// ---------- 约定的时间 ----------
export const getSchedules = () => schedules
export async function addSchedule(s) {
  const item = { id: uid(6), created: Date.now(), ...s }
  schedules.push(item)
  await serial('schedules', () => writeJSON(F.schedules, schedules))
  return item
}
export async function updateSchedules(fn) {
  schedules = fn(schedules)
  await serial('schedules', () => writeJSON(F.schedules, schedules))
}

// ---------- 推送订阅 ----------
export const getPush = () => push
export async function savePush(p) {
  push = p
  await serial('push', () => writeJSON(F.push, push))
}

// ---------- 登录会话 ----------
const SESSION_DAYS = 30
export function checkSession(token) {
  const s = token && sessions[token]
  return !!s && s.exp > Date.now()
}
export async function createSession() {
  const token = uid(32)
  const now = Date.now()
  for (const [k, v] of Object.entries(sessions)) if (v.exp < now) delete sessions[k]
  sessions[token] = { exp: now + SESSION_DAYS * 864e5 }
  await serial('sessions', () => writeJSON(F.sessions, sessions))
  return token
}
export async function dropSession(token) {
  delete sessions[token]
  await serial('sessions', () => writeJSON(F.sessions, sessions))
}

// ---------- 对话 ----------
const convPath = (id) => path.join(DIRS.convs, `${id.replace(/[^\w-]/g, '')}.json`)
const convCache = new Map()

export function listConversations() {
  const out = []
  for (const f of fs.readdirSync(DIRS.convs)) {
    if (!f.endsWith('.json')) continue
    const c = getConversation(f.slice(0, -5))
    if (!c) continue
    const last = [...c.messages].reverse().find((m) => !m.hidden && (m.role === 'user' || (m.role === 'assistant' && m.content)))
    out.push({
      id: c.id, title: c.title, createdAt: c.createdAt, updatedAt: c.updatedAt, unread: c.unread || 0,
      preview: last ? String(last.content).slice(0, 60) : '',
    })
  }
  return out.sort((a, b) => b.updatedAt - a.updatedAt)
}

export function getConversation(id) {
  if (!id) return null
  if (convCache.has(id)) return convCache.get(id)
  const c = readJSON(convPath(id), null)
  if (c) convCache.set(id, c)
  return c
}

export async function createConversation(title = '新的一页') {
  const now = Date.now()
  const c = { id: `${new Date(now).toISOString().slice(0, 10)}-${uid(6)}`, title, createdAt: now, updatedAt: now, unread: 0, summary: null, lastPromptTokens: 0, messages: [] }
  convCache.set(c.id, c)
  await saveConversation(c)
  return c
}

export function saveConversation(c) {
  c.updatedAt = Date.now()
  return serial(`conv:${c.id}`, () => writeJSON(convPath(c.id), c))
}

export async function deleteConversation(id) {
  convCache.delete(id)
  await serial(`conv:${id}`, () => fsp.rm(convPath(id), { force: true }))
}

export function addMessage(c, msg) {
  const m = { id: uid(8), ts: Date.now(), ...msg }
  c.messages.push(m)
  return m
}

export async function ensureMain() {
  let c = getConversation(state.mainId)
  if (!c) {
    const list = listConversations()
    c = list.length ? getConversation(list[0].id) : await createConversation('第一页')
    await patchState({ mainId: c.id })
  }
  return c
}

// ---------- 文件（截图、上传的图） ----------
export async function saveFile(buf, ext = 'jpg') {
  const id = `${Date.now().toString(36)}${uid(6)}.${ext}`
  await fsp.writeFile(path.join(DIRS.files, id), buf)
  return id
}
export const filePath = (id) => path.join(DIRS.files, path.basename(String(id)))

// ---------- 密钥（网页上填的 DeepSeek 密钥、改过的暗号） ----------
const SECRETS = path.join(DATA, 'secrets.json')
let secrets = readJSON(SECRETS, {})
export const getSecret = (k) => secrets[k]
export async function setSecret(k, v) {
  if (v === undefined || v === null || v === '') delete secrets[k]
  else secrets[k] = v
  await serial('secrets', async () => {
    await writeJSON(SECRETS, secrets)
    await fsp.chmod(SECRETS, 0o600).catch(() => {})
  })
}

// ---------- 朋友圈 ----------
const MOMENTS = path.join(DATA, 'moments.json')
let moments = readJSON(MOMENTS, [])
const saveMoments = () => serial('moments', () => writeJSON(MOMENTS, moments))
export const getMoments = () => moments
export async function addMoment(m) {
  const item = { id: uid(8), ts: Date.now(), liked: false, comments: [], images: [], ...m }
  moments.push(item)
  if (moments.length > 500) moments = moments.slice(-500)
  await saveMoments()
  return item
}
export async function updateMoment(id, fn) {
  const m = moments.find((x) => x.id === id)
  if (!m) return null
  fn(m)
  await saveMoments()
  return m
}
export async function removeMoment(id) {
  moments = moments.filter((x) => x.id !== id)
  await saveMoments()
}

// ---------- 闲逛记录 ----------
const LEISURE_LOG = path.join(DATA, 'leisure-log.json')
let leisureLog = readJSON(LEISURE_LOG, [])
export const getLeisureLog = () => leisureLog
export async function addLeisureLog(e) {
  leisureLog.push({ ts: Date.now(), ...e })
  if (leisureLog.length > 200) leisureLog = leisureLog.slice(-200)
  await serial('leisure-log', () => writeJSON(LEISURE_LOG, leisureLog))
}
