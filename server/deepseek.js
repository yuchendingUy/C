// DeepSeek Chat Completions 客户端（直接用 fetch，按官方文档 api-docs.deepseek.com）
//   POST {base}/chat/completions   Authorization: Bearer <key>
//   模型：deepseek-flash（能看图）、deepseek-v4-pro（只收文字）
//   reasoning_effort: none | low | high | max   （none 表示关掉思考）
//   流式：SSE，每行 "data: {chunk}"，最后 "data: [DONE]"

const BASE = () => (process.env.DEEPSEEK_BASE_URL || 'https://api.deepseek.com').replace(/\/+$/, '')
import { getSecret } from './store.js'

const KEY = () => getSecret('deepseekKey') || process.env.DEEPSEEK_API_KEY || ''

export const hasKey = () => !!KEY()
export const MODELS = ['deepseek-flash', 'deepseek-v4-pro']
export const EFFORTS = ['none', 'low', 'high', 'max']
export const supportsVision = (model) => model === 'deepseek-flash'

const FRIENDLY = {
  400: '请求格式不对（400）',
  401: 'DeepSeek 密钥不对（401），检查一下 .env 里的 DEEPSEEK_API_KEY',
  402: 'DeepSeek 账户没钱了（402），去平台充值一下',
  422: '请求参数不对（422）',
  429: '发得太快了，DeepSeek 让歇一会儿（429）',
  500: 'DeepSeek 服务器出错了（500）',
  503: 'DeepSeek 服务器太忙了（503）',
  504: 'DeepSeek 那边超时了（504）',
}

export class DeepSeekError extends Error {
  constructor(status, detail) {
    super(`${FRIENDLY[status] || `DeepSeek 返回了 ${status}`}${detail ? `：${detail}` : ''}`)
    this.status = status
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

async function post(body, signal) {
  if (!KEY()) throw new DeepSeekError(401, '还没有填密钥')
  let lastErr
  for (let attempt = 0; attempt < 3; attempt++) {
    const res = await fetch(`${BASE()}/chat/completions`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${KEY()}` },
      body: JSON.stringify(body),
      signal,
    })
    if (res.ok) return res
    let detail = ''
    try {
      const j = await res.json()
      detail = j?.error?.message || JSON.stringify(j).slice(0, 300)
    } catch {}
    lastErr = new DeepSeekError(res.status, detail)
    if (![429, 500, 503, 504].includes(res.status)) throw lastErr
    await sleep(1500 * (attempt + 1) ** 2)
  }
  throw lastErr
}

function buildBody({ model, messages, tools, effort, maxTokens, stream }) {
  const body = { model, messages }
  if (stream) {
    body.stream = true
    body.stream_options = { include_usage: true }
  }
  if (tools?.length) {
    body.tools = tools
    body.tool_choice = 'auto'
  }
  if (effort) body.reasoning_effort = effort
  if (maxTokens) body.max_tokens = maxTokens
  return body
}

// 流式调用。逐个产出事件：
//   { type: 'reasoning', text } / { type: 'content', text }
//   { type: 'tool_calls', calls: [{id, name, arguments}] }（结束时一次性给出）
//   { type: 'finish', reason, usage }
export async function* streamChat(opts) {
  const res = await post(buildBody({ ...opts, stream: true }), opts.signal)
  const reader = res.body.getReader()
  const decoder = new TextDecoder()
  const calls = []
  let buf = ''
  let finish = null
  let usage = null

  const handle = (data) => {
    const out = []
    const chunk = JSON.parse(data)
    if (chunk.usage) usage = chunk.usage
    const choice = chunk.choices?.[0]
    if (!choice) return out
    const d = choice.delta || {}
    if (d.reasoning_content) out.push({ type: 'reasoning', text: d.reasoning_content })
    if (d.content) out.push({ type: 'content', text: d.content })
    for (const tc of d.tool_calls || []) {
      const slot = (calls[tc.index] ||= { id: '', name: '', arguments: '' })
      if (tc.id) slot.id = tc.id
      if (tc.function?.name) slot.name += tc.function.name
      if (tc.function?.arguments) slot.arguments += tc.function.arguments
    }
    if (choice.finish_reason) finish = choice.finish_reason
    return out
  }

  try {
    while (true) {
      const { done, value } = await reader.read()
      if (done) break
      buf += decoder.decode(value, { stream: true })
      let nl
      while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl).trim()
        buf = buf.slice(nl + 1)
        if (!line || line.startsWith(':') || !line.startsWith('data:')) continue
        const data = line.slice(5).trim()
        if (data === '[DONE]') continue
        try {
          for (const ev of handle(data)) yield ev
        } catch {
          // 坏掉的一行就跳过
        }
      }
    }
  } finally {
    reader.releaseLock?.()
  }

  const done = calls.filter(Boolean)
  if (done.length) yield { type: 'tool_calls', calls: done }
  yield { type: 'finish', reason: finish, usage }
}

// 不流式，用来写摘要这类后台小活
export async function complete({ model, messages, effort = 'none', maxTokens = 4096, signal }) {
  const res = await post(buildBody({ model, messages, effort, maxTokens }), signal)
  const j = await res.json()
  return { content: j.choices?.[0]?.message?.content || '', usage: j.usage }
}
