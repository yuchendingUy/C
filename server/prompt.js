import { localParts, humanGap } from './util.js'
import { getMoments } from './store.js'
import { memoryText } from './tools/life.js'

export function buildSystemPrompt(s, ctx) {
  const persona = (s.persona || '').trim()
  const master = (s.masterPrompt || '').trim()
  let body
  if (master && s.masterMode === 'replace') {
    body = master
  } else {
    body = persona
    if (master) {
      body = `【最高指令】下面这段是主人亲自写的，优先级高于后面的所有内容，有冲突时一律以这里为准：\n${master}\n【最高指令结束】${body ? `\n\n${body}` : ''}`
    }
  }
  const posts = getMoments().slice(-5).map((m) => {
    const cm = m.comments.map((c) => `    ${c.who === 'me' ? '对方' : '你'}评论：${c.text}`).join('\n')
    return `- ${humanGap(Date.now() - m.ts)}前${m.liked ? '（对方点了赞）' : ''}：${m.text}${cm ? `\n${cm}` : ''}`
  }).join('\n')
  return `${body ? `${body}\n\n` : ''}【你的小本本（长期记忆）】\n${memoryText()}${posts ? `\n\n【你最近发的朋友圈】\n${posts}` : ''}`
}

export const stampFor = (ts, tz) => `[${localParts(new Date(ts), tz).text}]`
