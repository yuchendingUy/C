// "电脑操作"：用无头 Chromium（Playwright）开网页、看截图、点按钮、打字。
// Playwright 是可选安装的：没装时这些工具不会出现。
import { saveFile } from '../store.js'

let pw = null
try {
  pw = await import('playwright')
} catch {}

export const browserAvailable = !!pw

let browser = null
let page = null
let idleTimer = null

async function getPage() {
  clearTimeout(idleTimer)
  idleTimer = setTimeout(closeBrowser, 5 * 60 * 1000) // 5 分钟不用就关掉，省内存
  if (page && !page.isClosed()) return page
  if (!browser) {
    browser = await pw.chromium.launch({
      headless: true,
      args: ['--no-sandbox', '--disable-dev-shm-usage', '--disable-gpu'],
      ...(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {}),
    })
  }
  const context = await browser.newContext({ viewport: { width: 1280, height: 800 }, locale: 'zh-CN' })
  page = await context.newPage()
  return page
}

export async function closeBrowser() {
  clearTimeout(idleTimer)
  try { await browser?.close() } catch {}
  browser = null
  page = null
}

// 给页面上能点能填的东西编号，返回文字版的页面
async function snapshot(p) {
  const data = await p.evaluate(() => {
    document.querySelectorAll('[data-yz-ref]').forEach((el) => el.removeAttribute('data-yz-ref'))
    const sel = 'a[href],button,input,textarea,select,[role=button],[role=link],[role=tab],[role=checkbox],[role=menuitem],[contenteditable=true],summary,label[for]'
    const items = []
    let n = 0
    for (const el of document.querySelectorAll(sel)) {
      const r = el.getBoundingClientRect()
      const st = getComputedStyle(el)
      if (r.width < 2 || r.height < 2 || st.visibility === 'hidden' || st.display === 'none') continue
      if (r.bottom < 0 || r.top > innerHeight * 3) continue
      n++
      el.setAttribute('data-yz-ref', String(n))
      const tag = el.tagName.toLowerCase()
      const type = el.getAttribute('type') || ''
      const name = (el.getAttribute('aria-label') || el.innerText || el.value || el.getAttribute('placeholder') || el.getAttribute('title') || el.getAttribute('name') || '').trim().replace(/\s+/g, ' ').slice(0, 80)
      items.push(`[${n}] ${tag}${type ? `(${type})` : ''} ${name}${tag === 'a' ? ` → ${el.getAttribute('href').slice(0, 100)}` : ''}`)
      if (n >= 150) break
    }
    const text = (document.body?.innerText || '').replace(/\n{3,}/g, '\n\n').slice(0, 8000)
    return { title: document.title, url: location.href, items, text, scrollY: Math.round(scrollY), height: document.body?.scrollHeight || 0 }
  })
  return `标题：${data.title}\n网址：${data.url}\n滚动位置：${data.scrollY}/${data.height}\n\n可操作的元素（用编号点击或输入）：\n${data.items.join('\n') || '（没有）'}\n\n页面文字：\n${data.text}`
}

async function shot(p, ctx) {
  const buf = await p.screenshot({ type: 'jpeg', quality: 70 })
  const fileId = await saveFile(buf, 'jpg')
  const images = [{ fileId, dataUrl: `data:image/jpeg;base64,${buf.toString('base64')}` }]
  return { images, note: ctx.vision ? '截图在下面。' : '（截图已保存给对方看了，但当前模型看不了图，请依靠页面文字判断）' }
}

const after = async (p, ctx, withShot) => {
  await p.waitForLoadState('domcontentloaded', { timeout: 15000 }).catch(() => {})
  await p.waitForTimeout(600)
  const text = await snapshot(p)
  if (!withShot) return text
  const s = await shot(p, ctx)
  return { text: `${text}\n\n${s.note}`, images: s.images }
}

const refSel = (ref) => `[data-yz-ref="${Number(ref)}"]`

export const browserTools = !pw ? [] : [
  {
    name: 'browser_open',
    kind: 'read',
    label: '打开网页',
    description: '在服务器上的浏览器里打开网址，返回页面文字和带编号的可操作元素，并附一张截图。之后可以用 browser_click / browser_type 操作。',
    parameters: { type: 'object', properties: { url: { type: 'string' } }, required: ['url'] },
    summary: (a) => a.url,
    async run(a, ctx) {
      const p = await getPage()
      await p.goto(a.url, { timeout: 30000, waitUntil: 'domcontentloaded' })
      return after(p, ctx, true)
    },
  },
  {
    name: 'browser_look',
    kind: 'read',
    label: '看看屏幕',
    description: '重新获取当前网页的文字、元素编号和截图（页面变化后用）。',
    parameters: { type: 'object', properties: { screenshot: { type: 'boolean', description: '要不要截图，默认要' } } },
    summary: () => '当前页面',
    async run(a, ctx) {
      if (!page) return '还没有打开任何网页，先用 browser_open'
      return after(page, ctx, a.screenshot !== false)
    },
  },
  {
    name: 'browser_click',
    kind: 'write',
    label: '点一下',
    description: '点击网页上某个编号的元素（编号来自 browser_open / browser_look）。',
    parameters: { type: 'object', properties: { ref: { type: 'number', description: '元素编号' } }, required: ['ref'] },
    summary: (a) => `元素 [${a.ref}]`,
    async run(a, ctx) {
      if (!page) return '还没有打开任何网页'
      await page.click(refSel(a.ref), { timeout: 10000 })
      return after(page, ctx, true)
    },
  },
  {
    name: 'browser_type',
    kind: 'write',
    label: '打字',
    description: '在某个编号的输入框里输入文字（会先清空），submit=true 时输完按回车。',
    parameters: {
      type: 'object',
      properties: { ref: { type: 'number' }, text: { type: 'string' }, submit: { type: 'boolean' } },
      required: ['ref', 'text'],
    },
    summary: (a) => `在 [${a.ref}] 输入「${String(a.text).slice(0, 40)}」`,
    async run(a, ctx) {
      if (!page) return '还没有打开任何网页'
      await page.fill(refSel(a.ref), String(a.text), { timeout: 10000 })
      if (a.submit) await page.press(refSel(a.ref), 'Enter')
      return after(page, ctx, true)
    },
  },
  {
    name: 'browser_scroll',
    kind: 'read',
    label: '滚一滚',
    description: '上下滚动网页。',
    parameters: { type: 'object', properties: { direction: { type: 'string', enum: ['down', 'up', 'top', 'bottom'] } }, required: ['direction'] },
    summary: (a) => ({ down: '往下', up: '往上', top: '到顶', bottom: '到底' })[a.direction] || a.direction,
    async run(a, ctx) {
      if (!page) return '还没有打开任何网页'
      await page.evaluate((d) => {
        if (d === 'top') scrollTo(0, 0)
        else if (d === 'bottom') scrollTo(0, document.body.scrollHeight)
        else scrollBy(0, (d === 'up' ? -1 : 1) * innerHeight * 0.85)
      }, a.direction)
      return after(page, ctx, true)
    },
  },
  {
    name: 'browser_back',
    kind: 'read',
    label: '返回',
    description: '浏览器后退一页。',
    parameters: { type: 'object', properties: {} },
    summary: () => '上一页',
    async run(a, ctx) {
      if (!page) return '还没有打开任何网页'
      await page.goBack({ timeout: 15000 }).catch(() => {})
      return after(page, ctx, true)
    },
  },
  {
    name: 'browser_close',
    kind: 'read',
    label: '关浏览器',
    description: '用完关掉浏览器，释放内存。',
    parameters: { type: 'object', properties: {} },
    summary: () => '收工',
    async run() {
      await closeBrowser()
      return '浏览器关掉了'
    },
  },
]
