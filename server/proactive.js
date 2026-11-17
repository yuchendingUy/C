// 主动找你 —— 不按计划，随机得像个真人：
//  每分钟掷一次骰子，平均多久想起你一次由「话痨程度」决定（指数分布，间隔完全随机）。
//  你不回她，她会慢慢没那么频繁（但偶尔还是会来戳你）；你一回，马上恢复。
//  她晚上也睡觉，睡觉/起床时间每天随机浮动；起床后会来找你。
//  另外，她用 schedule_message 答应过的事，到点一定会来。
import { getSettings, getState, patchState, getSchedules, updateSchedules, getConversation, ensureMain, addMessage, saveConversation } from './store.js'
import { runTurn, isRunning } from './agent.js'
import { hasKey } from './deepseek.js'
import { broadcast } from './bus.js'
import { notifyAll } from './push.js'
import { localParts, partOfDay, humanGap, inWindow, hm } from './util.js'

const MIN = 60 * 1000
const H = 60 * MIN

// 话痨程度 → 平均多少分钟想起你一次
const MEAN_MINUTES = { 1: 240, 2: 120, 3: 60, 4: 30, 5: 14 }

const pick = (arr) => arr[Math.floor(Math.random() * arr.length)]

const IMPULSES = [
  '突然想起之前聊过的某件事，想接着说两句',
  '刚在服务器里溜达，看到一个小东西（某个日志、某个进程、硬盘、内存），想跟对方吐槽或者分享',
  '有点无聊，想找对方说说话',
  '冒出一个奇奇怪怪的念头，随口就说出来了',
  '想到一个好玩的问题，想问问对方',
  '就是想对方了，想撒个娇',
  '看了一眼现在的时间，顺口说一句跟这个时间有关的话（饭点、犯困、天黑了之类）',
  '随手发一句很短的话，比如"在吗"、"嘿"、"喂喂"、一个颜文字，或者一个词',
  '想跟对方说说自己刚刚在干嘛（你在服务器里的小日常）',
  '突然想夸夸对方，或者损对方一句',
  '有件小事想让对方猜',
  '想起对方之前说过要做的事，好奇怎么样了',
]

const IGNORED = [
  '对方还没回你上一条。你可以追问一句，或者假装不在意换个话题',
  '对方好一阵没回你了。有点小委屈，但别太黏，随便说一句就好',
  '对方一直没回。你可以小小生个气，或者自言自语一句',
  '对方没回你，可能在忙。轻轻戳一下就好，或者说一句"那我先去玩了"之类的话',
]

function dayJitter(st, date) {
  if (st.sleepJitter?.date === date) return st.sleepJitter
  const j = { date, start: Math.round((Math.random() - 0.5) * 80), end: Math.round((Math.random() - 0.5) * 80) }
  patchState({ sleepJitter: j })
  return j
}

function sleepWindow(s, st, lp) {
  const p = s.proactive
  const j = dayJitter(st, lp.date)
  const toHM = (min) => {
    const m = ((min % 1440) + 1440) % 1440
    return `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`
  }
  return { start: toHM(hm(p.sleepStart) + j.start), end: toHM(hm(p.sleepEnd) + j.end) }
}

export function isAsleep(date = new Date()) {
  const s = getSettings()
  const st = getState()
  const lp = localParts(date, s.timezone)
  const w = sleepWindow(s, st, lp)
  return inWindow(lp.hour * 60 + lp.minute, w.start, w.end)
}

// 你发消息时调用
export async function noteUserActivity() {
  const now = Date.now()
  const st = getState()
  const patch = { lastUserAt: now, unanswered: 0 }
  if (now - st.lastAffectionAt > 10 * MIN) {
    patch.affection = Math.min(100, st.affection + 1)
    patch.lastAffectionAt = now
  }
  await patchState(patch)
  broadcast({ type: 'stats', state: getState() })
}

function triggerText(kind, s, st, extra = {}) {
  const lp = localParts(new Date(), s.timezone)
  const who = s.userName === '你' ? '对方' : s.userName
  const since = st.lastUserAt ? `${who}上次跟你说话是 ${humanGap(Date.now() - st.lastUserAt)}前。` : ''
  const head = `【悄悄话：这条不是${who}发的，是系统提示，对方看不到】现在是${lp.text}（${partOfDay(lp.hour)}）。${since}`
  const tail = `像随手拿起手机发消息一样：别解释你为什么来找对方，别每次都问"在干嘛"，可以只发一句，最多 3 条短消息。要是真的觉得不合适，就只回 [SKIP]。`
  if (kind === 'reminder') {
    return `${head}\n到了你之前跟自己约好的时间，你当时的备注：「${extra.note}」\n去找${who}把这件事办了，需要的话可以用工具。说话还是平时的样子。`
  }
  if (kind === 'wake') {
    return `${head}\n你刚睡醒。迷迷糊糊地想找${who}说句话（早安也行，说梦也行，赖床也行，随你）。\n${tail}`
  }
  const ignored = st.unanswered > 0 ? `${pick(IGNORED)}。` : ''
  return `${head}\n${ignored}这会儿你${pick(IMPULSES)}。\n${tail}`
}

let busy = false

export async function fire(kind, extra = {}) {
  if (!hasKey()) return { ok: false, reason: '没有密钥' }
  const s = getSettings()
  const st = getState()
  const conv = (extra.convId && getConversation(extra.convId)) || (await ensureMain())
  if (isRunning(conv.id)) return { ok: false, reason: 'busy' }
  const trigger = addMessage(conv, { role: 'user', hidden: true, trigger: kind, content: triggerText(kind, s, st, extra) })
  await saveConversation(conv)
  const { produced, skipped } = await runTurn(conv, { proactive: true, triggerId: trigger.id })
  await patchState({ lastProactiveAt: Date.now() })
  if (skipped) return { ok: true, skipped: true }
  const said = produced.map((m) => m.content).filter(Boolean)
  if (!said.length) return { ok: true, skipped: true }
  conv.unread = (conv.unread || 0) + said.length
  await saveConversation(conv)
  if (kind !== 'reminder') await patchState({ unanswered: getState().unanswered + 1 })
  broadcast({ type: 'proactive', convId: conv.id, unread: conv.unread, kind })
  await notifyAll({ title: s.charName, body: said[0].replace(/\s+/g, ' ').slice(0, 90), convId: conv.id, tag: `yuzu-${conv.id}` })
  return { ok: true }
}

async function tick() {
  if (busy) return
  busy = true
  try {
    const s = getSettings()
    const st = getState()
    const now = Date.now()
    const lp = localParts(new Date(), s.timezone)

    // 好几天不理她，好感会慢慢掉一点
    if (st.lastDecayDate !== lp.date) {
      const patch = { lastDecayDate: lp.date }
      if (st.lastUserAt && now - st.lastUserAt > 36 * H) patch.affection = Math.max(0, st.affection - 1)
      await patchState(patch)
    }

    // 答应过的事，到点就来（睡觉也会爬起来）
    for (const sch of getSchedules().filter((x) => x.at <= now)) {
      const r = await fire('reminder', { note: sch.note, convId: sch.convId })
      if (r.reason === 'busy') continue
      await updateSchedules((list) => list.flatMap((x) => {
        if (x.id !== sch.id) return [x]
        if (x.repeat !== 'daily') return []
        let at = x.at
        while (at <= Date.now()) at += 24 * H
        return [{ ...x, at }]
      }))
    }

    const p = s.proactive
    if (!p.enabled || !st.lastUserAt) return // 还没见过面就不主动
    if (isAsleep()) return

    // 正聊得热乎就别插嘴
    const lastActive = Math.max(st.lastUserAt, st.lastProactiveAt)
    if (now - lastActive < 4 * MIN) return

    // 今天第一次醒来
    if (st.wokeDate !== lp.date) {
      const awakeFor = (lp.hour * 60 + lp.minute - hm(sleepWindow(s, st, lp).end) + 1440) % 1440
      if (awakeFor > 180) await patchState({ wokeDate: lp.date }) // 早就醒了，不算"刚睡醒"
      else if (Math.random() < 1 / 15) {
        await patchState({ wokeDate: lp.date })
        await fire('wake')
      }
      return
    }

    // 掷骰子：每分钟发生的概率 = 1 - e^(-1/平均分钟数)
    let mean = MEAN_MINUTES[Math.round(p.chattiness)] || 30
    mean *= 1 + 0.7 * st.unanswered // 不回她，她就慢慢不那么频繁
    if (st.unanswered >= 6) mean *= 3 // 一直不理，那她也会闹别扭
    mean = Math.min(mean, 10 * 60)
    if (Math.random() < 1 - Math.exp(-1 / mean)) await fire('idle')
  } catch (e) {
    console.error('[proactive]', e)
  } finally {
    busy = false
  }
}

export { tick as _tick }

export function startProactive() {
  setTimeout(tick, 20 * 1000)
  setInterval(tick, MIN)
}
