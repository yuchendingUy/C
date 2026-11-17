import fs from 'node:fs'
import { streamChat, complete, supportsVision } from './deepseek.js'
import { apiTools, getTool } from './tools/index.js'
import { buildSystemPrompt, stampFor } from './prompt.js'
import { getSettings, addMessage, saveConversation, filePath } from './store.js'
import { broadcast } from './bus.js'
import { uid, clip, estimateTokens } from './util.js'

const MAX_STEPS = 30
const running = new Map() // convId -> { ctrl }

export const isRunning = (id) => running.has(id)
export const runningIds = () => [...running.keys()]
export const stopTurn = (id) => running.get(id)?.ctrl.abort()

export const workdir = () => process.env.WORKDIR || process.env.HOME || '/root'
export const visionOn = (s) => s.vision === 'on' || (s.vision === 'auto' && supportsVision(s.model))

const dataUrl = (fileId) => {
  try {
    const buf = fs.readFileSync(filePath(fileId))
    const ext = fileId.split('.').pop().toLowerCase()
    const mime = ext === 'png' ? 'image/png' : ext === 'webp' ? 'image/webp' : ext === 'gif' ? 'image/gif' : 'image/jpeg'
    return `data:${mime};base64,${buf.toString('base64')}`
  } catch {
    return null
  }
}

// 把存下来的消息转成 DeepSeek 的 messages 格式
export function toApiMessages(conv, s, vision) {
  const tz = s.timezone
  const live = conv.messages.filter((m) => !m.archived && m.role !== 'event')
  // 只给最近的几张图，省 token
  const withImages = new Set()
  for (let i = live.length - 1, budget = 4; i >= 0 && budget > 0; i--) {
    if (live[i].images?.length) { withImages.add(live[i].id); budget-- }
  }
  const imageParts = (m) => (m.images || []).map(dataUrl).filter(Boolean).map((url) => ({ type: 'image_url', image_url: { url } }))

  const out = []
  for (let i = 0; i < live.length; i++) {
    const m = live[i]
    if (m.role === 'user') {
      const text = `${stampFor(m.ts, tz)} ${m.content || ''}`
      if (vision && withImages.has(m.id)) out.push({ role: 'user', content: [{ type: 'text', text }, ...imageParts(m)] })
      else out.push({ role: 'user', content: text + (m.images?.length ? `\n（发了 ${m.images.length} 张图片${vision ? '，是比较早的，已经省略' : '，当前模型看不了图'}）` : '') })
    } else if (m.role === 'assistant') {
      const calls = m.tool_calls || []
      const a = { role: 'assistant', content: m.content || (calls.length ? null : '') }
      if (m.reasoning_content) a.reasoning_content = m.reasoning_content
      if (calls.length) a.tool_calls = calls.map((c) => ({ id: c.id, type: 'function', function: { name: c.name, arguments: c.arguments || '{}' } }))
      out.push(a)
      // 每个工具调用都必须有对应的结果，中途断掉的补上一条
      const answered = new Set()
      let j = i + 1
      for (; j < live.length && live[j].role === 'tool'; j++) {
        const t = live[j]
        answered.add(t.tool_call_id)
        const text = t.content || ''
        if (vision && withImages.has(t.id)) out.push({ role: 'tool', tool_call_id: t.tool_call_id, content: [{ type: 'text', text }, ...imageParts(t)] })
        else out.push({ role: 'tool', tool_call_id: t.tool_call_id, content: text + (t.images?.length ? '\n（截图已省略）' : '') })
      }
      for (const c of calls) if (!answered.has(c.id)) out.push({ role: 'tool', tool_call_id: c.id, content: '（这一步被打断了，没有结果）' })
      i = j - 1
    }
    // 落单的 tool 消息直接丢掉
  }
  return out
}

function safeSummary(tool, args) {
  try { return String(tool.summary?.(args) ?? '').slice(0, 300) } catch { return '' }
}

async function executeTool(call, ctx) {
  const tool = getTool(call.name)
  if (!tool) return { text: `没有叫 ${call.name} 的工具` }
  let args
  try {
    args = call.arguments ? JSON.parse(call.arguments) : {}
  } catch {
    return { text: '错误：参数不是合法的 JSON，请检查后重新调用' }
  }
  ctx.emit({ type: 'tool_start', callId: call.id, name: tool.name, label: tool.label, summary: safeSummary(tool, args) })
  try {
    const r = await tool.run(args, ctx)
    return typeof r === 'string' ? { text: r } : { text: r.text || '', images: r.images }
  } catch (e) {
    return { text: `出错了：${e.message}` }
  }
}

// 跑一轮：调模型 → 有工具调用就执行 → 再调模型 …… 直到它不再调用工具
export async function runTurn(conv, { proactive = false, triggerId = null } = {}) {
  if (running.has(conv.id)) throw Object.assign(new Error(`${getSettings().charName}还在忙上一句呢`), { status: 409 })
  const ctrl = new AbortController()
  running.set(conv.id, { ctrl })
  const s = getSettings()
  const ctx = {
    settings: s, conv, proactive, vision: visionOn(s), signal: ctrl.signal, workdir: workdir(),
    emit: (e) => broadcast({ ...e, convId: conv.id }),
  }
  const produced = []
  let usedTools = false
  ctx.emit({ type: 'turn_start', proactive })
  try {
    await maybeCompact(conv, ctx)
    for (let step = 0; step < MAX_STEPS; step++) {
      const messages = [{ role: 'system', content: buildSystemPrompt(s, ctx) }]
      if (conv.summary?.text) {
        messages.push({ role: 'system', content: `【以前的回忆】更早的聊天记录已经整理成了下面这份笔记，当作你自己的记忆：\n${conv.summary.text}` })
      }
      messages.push(...toApiMessages(conv, s, ctx.vision))

      const id = uid(8)
      let content = ''
      let reasoning = ''
      let calls = []
      let usage = null
      ctx.emit({ type: 'assistant_start', id })
      for await (const ev of streamChat({ model: s.model, messages, tools: apiTools(), effort: s.effort, signal: ctrl.signal })) {
        if (ev.type === 'reasoning') { reasoning += ev.text; ctx.emit({ type: 'reasoning', id, text: ev.text }) }
        else if (ev.type === 'content') { content += ev.text; ctx.emit({ type: 'content', id, text: ev.text }) }
        else if (ev.type === 'tool_calls') calls = ev.calls
        else if (ev.type === 'finish') usage = ev.usage
      }
      if (usage?.prompt_tokens) conv.lastPromptTokens = usage.prompt_tokens + (usage.completion_tokens || 0)

      const msg = addMessage(conv, {
        id, role: 'assistant', content,
        ...(reasoning ? { reasoning_content: reasoning } : {}),
        ...(calls.length ? { tool_calls: calls } : {}),
        ...(proactive ? { proactive: true } : {}),
      })
      produced.push(msg)
      await saveConversation(conv)
      ctx.emit({ type: 'assistant_done', message: msg })
      if (!calls.length) break

      usedTools = true
      for (const call of calls) {
        if (ctrl.signal.aborted) break
        const r = await executeTool(call, ctx)
        const tm = addMessage(conv, {
          role: 'tool', tool_call_id: call.id, name: call.name, content: r.text,
          ...(r.images?.length ? { images: r.images.map((x) => x.fileId) } : {}),
        })
        await saveConversation(conv)
        ctx.emit({ type: 'tool_done', callId: call.id, message: tm })
      }
      if (ctrl.signal.aborted) break
      if (step === MAX_STEPS - 1) {
        const ev = addMessage(conv, { role: 'event', kind: 'info', content: `连续做了 ${MAX_STEPS} 步，先停下来喘口气。要继续就跟她说一声。` })
        ctx.emit({ type: 'event', message: ev })
      }
    }
    if (ctrl.signal.aborted) {
      const ev = addMessage(conv, { role: 'event', kind: 'info', content: '被你叫停了。' })
      ctx.emit({ type: 'event', message: ev })
    }
  } catch (e) {
    if (ctrl.signal.aborted || e.name === 'AbortError') {
      const ev = addMessage(conv, { role: 'event', kind: 'info', content: '被你叫停了。' })
      ctx.emit({ type: 'event', message: ev })
    } else {
      console.error('[turn]', e)
      const ev = addMessage(conv, { role: 'event', kind: 'error', content: e.message || String(e) })
      ctx.emit({ type: 'event', message: ev })
    }
  } finally {
    running.delete(conv.id)
  }

  // 主动消息：她觉得不该打扰的话就当没发生
  let skipped = false
  if (proactive) {
    const last = produced[produced.length - 1]
    if (last && /\[SKIP\]/i.test(last.content || '')) {
      if (!usedTools && produced.every((m) => !m.content || /^\s*\[SKIP\]\s*$/i.test(m.content))) {
        const ids = new Set(produced.map((m) => m.id))
        if (triggerId) ids.add(triggerId)
        conv.messages = conv.messages.filter((m) => !ids.has(m.id))
        skipped = true
      } else last.content = last.content.replace(/\[SKIP\]/gi, '').trim()
    }
  }
  await saveConversation(conv)
  ctx.emit({ type: 'turn_end', skipped })
  return { produced: skipped ? [] : produced, skipped }
}

// ---------------- 压缩对话 ----------------
// 思路来自 opencode 的 compaction：旧消息写成一份结构化笔记，最近的一段原样保留；已经有笔记时就在旧笔记上更新。

function transcript(msgs, s) {
  const lines = []
  for (const m of msgs) {
    const t = stampFor(m.ts, s.timezone)
    if (m.role === 'user') lines.push(m.hidden ? `${t}（系统提示）${clip(m.content, 300)}` : `${t} ${s.userName}：${m.content}${m.images?.length ? `（发了 ${m.images.length} 张图）` : ''}`)
    else if (m.role === 'assistant') {
      if (m.content) lines.push(`${t} ${s.charName}：${m.content}`)
      for (const c of m.tool_calls || []) lines.push(`${t} （${s.charName}调用工具 ${c.name}：${clip(c.arguments, 400)}）`)
    } else if (m.role === 'tool') lines.push(`（工具结果：${clip(m.content, 500)}）`)
  }
  return lines.join('\n')
}

function summaryPrompt(s, previous) {
  return `你在帮${s.charName}整理记忆。上面是${s.charName}和${s.userName}的一段聊天记录${previous ? '，以及之前已经整理好的旧笔记' : ''}。
${previous ? '把旧笔记和新的聊天合并成一份新笔记。新内容优先；旧笔记里没过时的都保留，已经解决或过时的可以删掉。' : '把这段聊天整理成一份笔记，之后它会代替原始记录，成为' + s.charName + '的记忆。'}

必须用下面的格式（不适用的部分写"（无）"），不要加开场白或解释：

## 关于${s.userName}
- 对方的信息、喜好、习惯、最近的状态和心情

## 我们之间
- 关系进展、彼此的称呼、只有你们懂的梗、语气和相处方式

## 聊过的事
- 按时间顺序，简短写重要的话题和结论

## 在服务器上做过的事
- 做了什么、改了哪些文件、装了什么、现在的状态

## 没做完的事和约定
- 答应过但还没做的事、对方还没得到回答的问题（尽量保留原话）

规则：
- 每条一行，简短，不写大段。
- 文件路径、命令、报错、网址、数字、日期一字不差地保留。
- 不要提"总结""压缩"这些事。`
}

export async function compactConversation(conv, { manual = false, emit } = {}) {
  const s = getSettings()
  const live = conv.messages.filter((m) => !m.archived && m.role !== 'event')
  if (live.length < 4) return { ok: false, reason: '聊得还不多，不用整理' }
  // 从后往前保留 keepRecent 个 token 的内容，切点放在一条用户消息前面，保证工具调用不被拆开
  let acc = 0
  let cut = live.length
  for (let i = live.length - 1; i >= 0; i--) {
    acc += estimateTokens(live[i].content) + estimateTokens(live[i].reasoning_content || '') / 2
    if (acc > s.keepRecent) { cut = i + 1; break }
    cut = i
  }
  while (cut < live.length && live[cut].role !== 'user') cut++
  if (manual && cut <= 1) {
    // 手动整理但内容不多：只原样留下最后两轮
    const users = live.map((m, i) => (m.role === 'user' ? i : -1)).filter((i) => i >= 0)
    cut = users.length >= 3 ? users[users.length - 2] : 0
  }
  if (cut >= live.length) {
    // 最近一段太长了：至少保留最后一条用户消息开始的内容
    cut = live.findLastIndex((m) => m.role === 'user')
  }
  if (cut <= 1) return { ok: false, reason: '没有足够旧的内容可以整理' }
  const old = live.slice(0, cut)

  const messages = [
    { role: 'system', content: `你是一个细心的记录员，负责把聊天记录整理成简洁、准确的中文笔记。` },
    {
      role: 'user',
      content: `${conv.summary?.text ? `【旧笔记】\n${conv.summary.text}\n\n` : ''}【聊天记录】\n${clip(transcript(old, s), 300000)}\n\n${summaryPrompt(s, !!conv.summary?.text)}`,
    },
  ]
  const { content } = await complete({ model: s.model, messages, effort: 'none', maxTokens: 6000 })
  if (!content.trim()) throw new Error('整理记忆失败：模型没有返回内容')
  const ids = new Set(old.map((m) => m.id))
  for (const m of conv.messages) if (ids.has(m.id) || (m.role === 'event' && !m.archived && m.ts <= old[old.length - 1].ts)) m.archived = true
  conv.summary = { text: content.trim(), ts: Date.now(), count: (conv.summary?.count || 0) + old.length }
  conv.lastPromptTokens = 0
  const ev = addMessage(conv, { role: 'event', kind: 'compact', content: `前面的 ${old.length} 条消息折成了一页日记${manual ? '' : '（聊太长了自动整理的）'}` })
  await saveConversation(conv)
  emit?.({ type: 'compacted', convId: conv.id, message: ev })
  return { ok: true, count: old.length }
}

async function maybeCompact(conv, ctx) {
  const s = ctx.settings
  const live = conv.messages.filter((m) => !m.archived && m.role !== 'event')
  const est = conv.lastPromptTokens || live.reduce((n, m) => n + estimateTokens(m.content) + estimateTokens(m.reasoning_content || ''), 0)
  if (est < s.compactAt) return
  ctx.emit({ type: 'compacting' })
  try {
    await compactConversation(conv, { emit: (e) => broadcast(e) })
  } catch (e) {
    console.error('[compact]', e)
  }
}
