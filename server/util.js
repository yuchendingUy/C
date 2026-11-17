import fs from 'node:fs'
import fsp from 'node:fs/promises'
import path from 'node:path'
import crypto from 'node:crypto'
import { fileURLToPath } from 'node:url'

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')

// 读 .env（不依赖第三方库）。已经存在的环境变量优先。
export function loadEnv(file = path.join(ROOT, '.env')) {
  if (!fs.existsSync(file)) return
  for (const raw of fs.readFileSync(file, 'utf8').split(/\r?\n/)) {
    const line = raw.trim()
    if (!line || line.startsWith('#')) continue
    const eq = line.indexOf('=')
    if (eq < 0) continue
    const key = line.slice(0, eq).trim()
    let val = line.slice(eq + 1).trim()
    if ((val.startsWith('"') && val.endsWith('"')) || (val.startsWith("'") && val.endsWith("'"))) val = val.slice(1, -1)
    if (process.env[key] === undefined) process.env[key] = val
  }
}

export const uid = (n = 10) => crypto.randomBytes(n).toString('base64url').slice(0, n)

export function readJSON(file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'))
  } catch {
    return typeof fallback === 'function' ? fallback() : structuredClone(fallback)
  }
}

// 先写临时文件再改名，断电也不会写坏
export async function writeJSON(file, data) {
  await fsp.mkdir(path.dirname(file), { recursive: true })
  const tmp = `${file}.${process.pid}.${uid(6)}.tmp`
  await fsp.writeFile(tmp, JSON.stringify(data, null, 1))
  await fsp.rename(tmp, file)
}

// 同一个 key 的任务排队执行，避免同时写同一个文件
const queues = new Map()
export function serial(key, fn) {
  const prev = queues.get(key) || Promise.resolve()
  const next = prev.then(fn, fn)
  queues.set(key, next.catch(() => {}))
  return next
}

export function clip(text, max) {
  text = String(text ?? '')
  if (text.length <= max) return text
  const head = Math.floor(max * 0.7)
  const tail = max - head
  return `${text.slice(0, head)}\n\n……（中间省略了 ${text.length - max} 个字符）……\n\n${text.slice(-tail)}`
}

// 粗略估算 token：中日韩字符约 0.6，其它约 0.27
export function estimateTokens(value) {
  const s = typeof value === 'string' ? value : JSON.stringify(value ?? '')
  let cjk = 0
  for (const ch of s) if (/[　-鿿가-힯＀-￯]/.test(ch)) cjk++
  return Math.ceil(cjk * 0.6 + (s.length - cjk) * 0.27)
}

// 按设置里的时区算本地时间
export function localParts(date = new Date(), timeZone = 'Asia/Shanghai') {
  const f = new Intl.DateTimeFormat('zh-CN', {
    timeZone, year: 'numeric', month: '2-digit', day: '2-digit',
    hour: '2-digit', minute: '2-digit', weekday: 'long', hour12: false,
  })
  const p = Object.fromEntries(f.formatToParts(date).map((x) => [x.type, x.value]))
  const hour = Number(p.hour) % 24
  return {
    date: `${p.year}-${p.month}-${p.day}`,
    time: `${String(hour).padStart(2, '0')}:${p.minute}`,
    hour,
    minute: Number(p.minute),
    weekday: p.weekday,
    text: `${p.year}年${Number(p.month)}月${Number(p.day)}日 ${p.weekday} ${String(hour).padStart(2, '0')}:${p.minute}`,
  }
}

export function partOfDay(hour) {
  if (hour < 5) return '深夜'
  if (hour < 9) return '早上'
  if (hour < 12) return '上午'
  if (hour < 14) return '中午'
  if (hour < 18) return '下午'
  if (hour < 20) return '傍晚'
  return '晚上'
}

export function humanGap(ms) {
  const m = Math.round(ms / 60000)
  if (m < 60) return `${m} 分钟`
  const h = Math.round(m / 6) / 10
  if (h < 48) return `${h} 小时`
  return `${Math.round(h / 24)} 天`
}

// 把 "HH:MM" 转成当天的分钟数
export const hm = (s) => {
  const [h, m] = String(s || '0:0').split(':').map(Number)
  return (h || 0) * 60 + (m || 0)
}

export function inWindow(nowMin, start, end) {
  const a = hm(start), b = hm(end)
  if (a === b) return false
  return a < b ? nowMin >= a && nowMin < b : nowMin >= a || nowMin < b
}

// 某个时区里 "YYYY-MM-DD HH:MM" 对应的真实时间
export function zonedToDate(str, timeZone) {
  const m = String(str).trim().match(/^(\d{4})-(\d{1,2})-(\d{1,2})[ T](\d{1,2}):(\d{2})/)
  if (!m) return null
  const [, y, mo, d, h, mi] = m.map(Number)
  const guess = Date.UTC(y, mo - 1, d, h, mi)
  // 用这个时区在 guess 时刻的偏移来修正（夏令时边界误差可接受）
  const p = localParts(new Date(guess), timeZone)
  const asLocal = Date.UTC(...p.date.split('-').map((v, i) => (i === 1 ? Number(v) - 1 : Number(v))), p.hour, p.minute)
  return new Date(guess - (asLocal - guess))
}
