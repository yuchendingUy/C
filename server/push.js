// Web Push：网页关着的时候也能收到"柚柚找你"的通知（需要 HTTPS 访问）
import { getPush, savePush } from './store.js'

let webpush = null
try {
  webpush = (await import('web-push')).default
} catch {}

export const pushAvailable = () => !!webpush

export async function initPush() {
  if (!webpush) return
  const p = getPush()
  if (!p.vapid) {
    p.vapid = webpush.generateVAPIDKeys()
    await savePush(p)
  }
  webpush.setVapidDetails('mailto:yuzu@localhost', p.vapid.publicKey, p.vapid.privateKey)
}

export const publicKey = () => getPush().vapid?.publicKey || null

export async function subscribe(sub) {
  if (!sub?.endpoint) throw new Error('订阅信息不对')
  const p = getPush()
  p.subs = p.subs.filter((s) => s.endpoint !== sub.endpoint)
  p.subs.push(sub)
  await savePush(p)
}

export async function unsubscribe(endpoint) {
  const p = getPush()
  p.subs = p.subs.filter((s) => s.endpoint !== endpoint)
  await savePush(p)
}

export async function notifyAll(payload) {
  if (!webpush) return
  const p = getPush()
  if (!p.subs.length) return
  const dead = new Set()
  await Promise.all(p.subs.map(async (sub) => {
    try {
      await webpush.sendNotification(sub, JSON.stringify(payload), { TTL: 3600 * 12 })
    } catch (e) {
      if (e.statusCode === 404 || e.statusCode === 410) dead.add(sub.endpoint)
      else console.error('[push]', e.statusCode || '', e.message)
    }
  }))
  if (dead.size) {
    p.subs = p.subs.filter((s) => !dead.has(s.endpoint))
    await savePush(p)
  }
}
