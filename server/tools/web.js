import { clip } from '../util.js'

const ENT = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ' }

export function htmlToText(html) {
  let s = String(html)
  s = s.replace(/<(script|style|noscript|svg|head)[\s\S]*?<\/\1>/gi, '')
  s = s.replace(/<a\s[^>]*href=["']([^"']+)["'][^>]*>([\s\S]*?)<\/a>/gi, (_, href, t) => `${t.replace(/<[^>]+>/g, '').trim()} (${href})`)
  s = s.replace(/<(h[1-6])[^>]*>/gi, '\n\n# ').replace(/<\/(h[1-6])>/gi, '\n')
  s = s.replace(/<li[^>]*>/gi, '\n- ')
  s = s.replace(/<(br|\/p|\/div|\/tr|\/section|\/article|\/li)[^>]*>/gi, '\n')
  s = s.replace(/<[^>]+>/g, '')
  s = s.replace(/&(#x?[0-9a-f]+|\w+);/gi, (m, e) => {
    if (e[0] === '#') {
      const n = e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10)
      return Number.isFinite(n) ? String.fromCodePoint(n) : m
    }
    return ENT[e.toLowerCase()] ?? m
  })
  return s.replace(/[ \t]+\n/g, '\n').replace(/\n{3,}/g, '\n\n').replace(/[ \t]{2,}/g, ' ').trim()
}

export const webTools = [
  {
    name: 'webfetch',
    kind: 'read',
    label: '上网看看',
    description: '打开一个网址并读取内容（网页会转成纯文字）。适合看文档、新闻、API 返回。需要点击、登录、执行 JS 的网页用 browser_* 工具。',
    parameters: {
      type: 'object',
      properties: {
        url: { type: 'string', description: 'http 或 https 网址' },
        raw: { type: 'boolean', description: '返回原始内容（不把 HTML 转成文字）' },
      },
      required: ['url'],
    },
    summary: (a) => a.url,
    async run(a, ctx) {
      if (!/^https?:\/\//i.test(a.url)) return '错误：网址要以 http:// 或 https:// 开头'
      const signal = AbortSignal.any([AbortSignal.timeout(30000), ctx.signal].filter(Boolean))
      const res = await fetch(a.url, {
        signal,
        redirect: 'follow',
        headers: { 'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130 Safari/537.36', Accept: 'text/html,application/json,text/plain,*/*' },
      })
      const type = res.headers.get('content-type') || ''
      if (!/text|json|xml|javascript/.test(type)) return `状态 ${res.status}，内容类型 ${type}，不是文本，没法读`
      const body = await res.text()
      const text = !a.raw && /html/.test(type) ? htmlToText(body) : body
      return `状态 ${res.status}  ${res.url}\n\n${clip(text, 40000)}`
    },
  },
]
