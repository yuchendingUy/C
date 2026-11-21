// 闲逛：她闲着没事的时候，自己去做设置里写的那些事（一句话就行，不用网址），
// 看到有意思的就发朋友圈。间隔随机，平均多久一次看设置。睡着的时候不逛。
import { getSettings, getMoments, updateMoment, getLeisureLog, addLeisureLog } from './store.js'
import { runTurn } from './agent.js'
import { complete, hasKey } from './deepseek.js'
import { buildSystemPrompt } from './prompt.js'
import { broadcast } from './bus.js'
import { isAsleep } from './proactive.js'
import { workdir } from './agent.js'
import { localParts, partOfDay, humanGap, uid } from './util.js'

let busy = false
let nextAt = 0
let current = null
export const leisureStatus = () => current

const pick = (arr) => arr[Math.floor(Math.random() * arr.length)]

// 指数分布的随机间隔：平均 everyMin 分钟，最短 1 分钟
function rollNext() {
  const mean = Math.max(1, Number(getSettings().leisure?.everyMin) || 6)
  return Date.now() + Math.max(1, -Math.log(1 - Math.random()) * mean) * 60000
}

function outingPrompt(activity) {
  const s = getSettings()
  const lp = localParts(new Date(), s.timezone)
  const seen = getLeisureLog().slice(-12).map((e) => `- ${humanGap(Date.now() - e.ts)}前（${e.activity}）：${e.note || '没记'}`).join('\n') || '（还没有）'
  const recent = getMoments().slice(-6).map((m) => `- ${humanGap(Date.now() - m.ts)}前：${m.text}`).join('\n') || '（还没发过）'
  const lastPost = getMoments().at(-1)
  return `【这是你自己的空闲时间，对方看不到这段】现在是${lp.text}（${partOfDay(lp.hour)}）。
你闲着没事，想去做这件事：「${activity}」
用工具自己去做。网址你自己想办法找，不知道就先上网搜。挑最新的、你之前没看过的东西看。

你最近闲逛时已经看过的（别重复）：
${seen}

你最近发过的朋友圈：
${recent}
${lastPost ? `（上一条是 ${humanGap(Date.now() - lastPost.ts)}前发的）` : ''}

看完以后：要是看到真正让你有感触、觉得好玩或者想吐槽的东西，就用 post_moment 发一条朋友圈，像真人发朋友圈那样，一两句自己的感受，合适就配上截图；平平无奇就不发。一次最多发一条。
最后用一句话写下这次看到了什么，留给以后的自己看。`
}

export async function goOut(activity) {
  const s = getSettings()
  if (busy || !hasKey()) return false
  activity = activity || pick(s.leisure?.activities || [])
  if (!activity) return false
  busy = true
  current = { activity, since: Date.now() }
  broadcast({ type: 'leisure', active: true, activity })
  const conv = { id: `leisure-${uid(6)}`, title: '', messages: [], summary: null }
  conv.messages.push({ id: uid(8), ts: Date.now(), role: 'user', hidden: true, content: outingPrompt(activity) })
  try {
    const { produced } = await runTurn(conv, { ephemeral: true, maxSteps: 10, browserKey: 'leisure', effort: 'low', extra: { activity } })
    const err = conv.messages.findLast((m) => m.role === 'event' && m.kind === 'error')
    const note = (err ? `出错了：${err.content}` : produced.at(-1)?.content || '').replace(/\s+/g, ' ').trim().slice(0, 200)
    const posted = conv.messages.some((m) => m.role === 'tool' && m.name === 'post_moment')
    await addLeisureLog({ activity, note, posted })
  } catch (e) {
    console.error('[leisure]', e.message)
    await addLeisureLog({ activity, note: `出错了：${e.message}`.slice(0, 200), posted: false })
  } finally {
    busy = false
    current = null
    broadcast({ type: 'leisure', active: false })
  }
  return true
}

// 你评论了她的朋友圈，她来回复
export async function replyToComment(momentId) {
  const s = getSettings()
  const m = getMoments().find((x) => x.id === momentId)
  if (!m || !hasKey()) return
  const who = s.userName || '对方'
  const thread = m.comments.map((c) => `${c.who === 'me' ? who : '你'}：${c.text}`).join('\n')
  const prompt = `【朋友圈】你在${humanGap(Date.now() - m.ts)}前发了一条朋友圈：「${m.text}」${m.images?.length ? '（配了一张截图）' : ''}
评论区：
${thread}

${who}刚刚评论了你。像在朋友圈回复评论一样，回一句短短的话。只输出回复的内容，不要加引号，不要分段。`
  try {
    const { content } = await complete({
      model: s.model,
      messages: [{ role: 'system', content: buildSystemPrompt(s, { workdir: workdir() }) }, { role: 'user', content: prompt }],
      effort: 'none',
      maxTokens: 300,
    })
    const text = content.replace(/^["“「]|["”」]$/g, '').trim().slice(0, 500)
    if (!text) return
    const updated = await updateMoment(momentId, (x) => x.comments.push({ id: uid(6), who: 'her', text, ts: Date.now() }))
    broadcast({ type: 'moment_update', moment: updated })
  } catch (e) {
    console.error('[moment reply]', e.message)
  }
}

async function tick() {
  const L = getSettings().leisure
  if (!L?.enabled || !L.activities?.length || busy) return
  if (isAsleep()) return
  if (!nextAt) { nextAt = rollNext(); return }
  if (Date.now() < nextAt) return
  nextAt = rollNext()
  await goOut()
}

export function startLeisure() {
  setInterval(tick, 20 * 1000)
}
