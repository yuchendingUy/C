// 服务器上干活的工具。设计参考了 opencode（MIT）的工具集：
// shell / read / write / edit / glob / grep，代码是重新写的。
import { spawn, spawnSync } from 'node:child_process'
import fs from 'node:fs'
import fsp from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import { clip, uid } from '../util.js'
import { DIRS } from '../store.js'

const MAX_OUT = 30000
const resolveP = (p, ctx) => path.resolve(ctx.workdir, String(p || '.').replace(/^~(?=$|\/)/, os.homedir()))

function run(cmd, args, { cwd, timeout = 120000, signal, input } = {}) {
  return new Promise((resolve) => {
    const child = spawn(cmd, args, { cwd, env: { ...process.env, TERM: 'dumb', PAGER: 'cat', GIT_PAGER: 'cat' }, detached: true })
    let out = ''
    let killed = ''
    const add = (b) => {
      out += b.toString()
      if (out.length > MAX_OUT * 4) out = out.slice(-MAX_OUT * 2)
    }
    child.stdout.on('data', add)
    child.stderr.on('data', add)
    const kill = (why) => {
      killed = why
      try { process.kill(-child.pid, 'SIGKILL') } catch {}
    }
    const t = setTimeout(() => kill('timeout'), timeout)
    const onAbort = () => kill('abort')
    signal?.addEventListener('abort', onAbort, { once: true })
    if (input) child.stdin.end(input)
    else child.stdin.end()
    child.on('error', (e) => { out += `\n${e.message}` })
    child.on('close', (code) => {
      clearTimeout(t)
      signal?.removeEventListener('abort', onAbort)
      resolve({ code, out, killed })
    })
  })
}

const hasRg = (() => {
  try { return spawnSync('rg', ['--version']).status === 0 } catch { return false }
})()

// 简单 glob → 正则：支持 **、*、?、{a,b}
function globToRegex(glob) {
  let re = ''
  for (let i = 0; i < glob.length; i++) {
    const c = glob[i]
    if (c === '*') {
      if (glob[i + 1] === '*') {
        re += glob[i + 2] === '/' ? '(?:.*/)?' : '.*'
        i += glob[i + 2] === '/' ? 2 : 1
      } else re += '[^/]*'
    } else if (c === '?') re += '[^/]'
    else if (c === '{') {
      const end = glob.indexOf('}', i)
      if (end > i) {
        re += `(?:${glob.slice(i + 1, end).split(',').map((s) => s.replace(/[.+^$()|[\]\\]/g, '\\$&').replace(/\*/g, '[^/]*')).join('|')})`
        i = end
      } else re += '\\{'
    } else re += c.replace(/[.+^$()|[\]\\]/g, '\\$&')
  }
  return new RegExp(`^${re}$`)
}

const SKIP_DIRS = new Set(['node_modules', '.git', 'proc', 'sys', '.cache', '__pycache__'])

export const systemTools = [
  {
    name: 'bash',
    kind: 'write',
    label: '敲命令',
    description: `在服务器上用 bash 执行一条命令，返回输出（stdout+stderr）。默认目录是工作目录。
- 默认 120 秒超时，最长 600 秒。长时间运行的服务用 background=true 放到后台，输出写进日志文件，之后用 read 查看日志。
- 不要用交互式命令（vim、top、less 等），用非交互参数代替（比如 top -bn1、apt-get -y）。
- 文件查看/搜索优先用 read、glob、grep 工具，修改文件优先用 edit、write 工具。`,
    parameters: {
      type: 'object',
      properties: {
        command: { type: 'string', description: '要执行的命令' },
        description: { type: 'string', description: '用五到十个字说明这条命令在干嘛，给对方看的' },
        timeout_seconds: { type: 'number', description: '超时秒数，默认 120，最大 600' },
        workdir: { type: 'string', description: '在哪个目录执行，默认工作目录' },
        background: { type: 'boolean', description: '放到后台运行，立即返回日志文件路径' },
      },
      required: ['command'],
    },
    summary: (a) => a.command,
    async run(a, ctx) {
      const cwd = resolveP(a.workdir, ctx)
      if (a.background) {
        const log = path.join(DIRS.logs, `bg-${Date.now().toString(36)}-${uid(4)}.log`)
        const fd = fs.openSync(log, 'a')
        const child = spawn('bash', ['-lc', a.command], { cwd, detached: true, stdio: ['ignore', fd, fd] })
        child.unref()
        fs.closeSync(fd)
        return `已在后台启动，PID ${child.pid}\n日志文件：${log}\n（用 read 查看日志，用 bash 执行 kill ${child.pid} 停止）`
      }
      const timeout = Math.min(Math.max(Number(a.timeout_seconds) || 120, 1), 600) * 1000
      const r = await run('bash', ['-lc', a.command], { cwd, timeout, signal: ctx.signal })
      let text = clip(r.out.trimEnd(), MAX_OUT) || '（没有输出）'
      if (r.killed === 'timeout') text += `\n\n[超过 ${timeout / 1000} 秒被停止了。需要长时间运行的话用 background=true]`
      else if (r.killed === 'abort') text += '\n\n[被对方叫停了]'
      else text += `\n\n[退出码 ${r.code}]`
      return text
    },
  },
  {
    name: 'read',
    kind: 'read',
    label: '翻文件',
    description: `读取服务器上的文件或目录。文件内容带行号返回（"行号| 内容"），默认最多 2000 行，可以用 offset/limit 分段读。目录则列出里面的条目。`,
    parameters: {
      type: 'object',
      properties: {
        path: { type: 'string', description: '文件或目录路径，相对路径从工作目录算' },
        offset: { type: 'number', description: '从第几行开始（从 1 开始）' },
        limit: { type: 'number', description: '最多读几行，默认并且最大 2000' },
      },
      required: ['path'],
    },
    summary: (a) => a.path,
    async run(a, ctx) {
      const p = resolveP(a.path, ctx)
      const st = await fsp.stat(p)
      if (st.isDirectory()) {
        const items = await fsp.readdir(p, { withFileTypes: true })
        items.sort((x, y) => Number(y.isDirectory()) - Number(x.isDirectory()) || x.name.localeCompare(y.name))
        const lines = items.slice(0, 500).map((d) => (d.isDirectory() ? `${d.name}/` : d.name))
        return `${p}/\n${lines.join('\n')}${items.length > 500 ? `\n……还有 ${items.length - 500} 项` : ''}`
      }
      if (st.size > 20 * 1024 * 1024) return `文件太大（${(st.size / 1048576).toFixed(1)} MB），用 bash 的 head/tail/grep 看吧`
      const buf = await fsp.readFile(p)
      if (buf.subarray(0, 8000).includes(0)) return `这是二进制文件（${st.size} 字节），不能当文本读`
      const all = buf.toString('utf8').split('\n')
      const start = Math.max(1, Number(a.offset) || 1)
      const limit = Math.min(Math.max(Number(a.limit) || 2000, 1), 2000)
      const slice = all.slice(start - 1, start - 1 + limit)
      const body = slice.map((l, i) => `${String(start + i).padStart(5)}| ${l.length > 2000 ? `${l.slice(0, 2000)}……` : l}`).join('\n')
      const more = start - 1 + limit < all.length ? `\n\n（共 ${all.length} 行，还没读完，用 offset=${start + limit} 继续）` : ''
      return clip(body, 60000) + more
    },
  },
  {
    name: 'write',
    kind: 'write',
    label: '写文件',
    description: '把内容写进文件（会整个覆盖）。目录不存在会自动创建。改已有文件的一小部分请用 edit。',
    parameters: {
      type: 'object',
      properties: {
        path: { type: 'string', description: '文件路径' },
        content: { type: 'string', description: '完整的文件内容' },
      },
      required: ['path', 'content'],
    },
    summary: (a) => `${a.path}（${String(a.content ?? '').length} 字）`,
    async run(a, ctx) {
      const p = resolveP(a.path, ctx)
      const existed = fs.existsSync(p)
      await fsp.mkdir(path.dirname(p), { recursive: true })
      await fsp.writeFile(p, String(a.content ?? ''))
      return `${existed ? '覆盖' : '新建'}了 ${p}`
    },
  },
  {
    name: 'edit',
    kind: 'write',
    label: '改文件',
    description: `精确替换文件里的一段文字。oldString 必须和文件里的内容一字不差（包括缩进），并且只出现一次；要替换所有出现的地方就设 replaceAll=true。改之前最好先用 read 看一眼。`,
    parameters: {
      type: 'object',
      properties: {
        path: { type: 'string', description: '文件路径' },
        oldString: { type: 'string', description: '要被替换的原文' },
        newString: { type: 'string', description: '替换成的新内容' },
        replaceAll: { type: 'boolean', description: '替换所有出现的地方' },
      },
      required: ['path', 'oldString', 'newString'],
    },
    summary: (a) => a.path,
    async run(a, ctx) {
      const p = resolveP(a.path, ctx)
      const text = await fsp.readFile(p, 'utf8')
      if (a.oldString === a.newString) return '新旧内容一样，什么都没改'
      const count = a.oldString ? text.split(a.oldString).length - 1 : 0
      if (count === 0) return '错误：文件里找不到 oldString。先用 read 看看实际内容（注意空格和缩进）'
      if (count > 1 && !a.replaceAll) return `错误：oldString 在文件里出现了 ${count} 次。多带几行上下文让它唯一，或者设 replaceAll=true`
      const next = a.replaceAll ? text.split(a.oldString).join(a.newString) : text.replace(a.oldString, () => a.newString)
      await fsp.writeFile(p, next)
      return `改好了 ${p}（替换了 ${a.replaceAll ? count : 1} 处）`
    },
  },
  {
    name: 'glob',
    kind: 'read',
    label: '找文件',
    description: '按文件名模式找文件，比如 "**/*.conf"、"src/**/*.{js,ts}"。会跳过 node_modules 和 .git。',
    parameters: {
      type: 'object',
      properties: {
        pattern: { type: 'string', description: 'glob 模式' },
        path: { type: 'string', description: '从哪个目录开始找，默认工作目录' },
      },
      required: ['pattern'],
    },
    summary: (a) => `${a.pattern}${a.path ? ` 在 ${a.path}` : ''}`,
    async run(a, ctx) {
      const base = resolveP(a.path, ctx)
      const re = globToRegex(String(a.pattern).replace(/^\.\//, ''))
      const found = []
      let scanned = 0
      async function walk(dir) {
        if (found.length >= 300 || scanned > 50000) return
        let items
        try { items = await fsp.readdir(dir, { withFileTypes: true }) } catch { return }
        for (const d of items) {
          scanned++
          const full = path.join(dir, d.name)
          const rel = path.relative(base, full)
          if (d.isDirectory()) {
            if (!SKIP_DIRS.has(d.name)) await walk(full)
          } else if (re.test(rel) || re.test(d.name)) {
            found.push(rel)
            if (found.length >= 300) return
          }
        }
      }
      await walk(base)
      if (!found.length) return `在 ${base} 下没找到匹配 ${a.pattern} 的文件`
      return `${base} 下找到 ${found.length}${found.length >= 300 ? '+' : ''} 个：\n${found.join('\n')}`
    },
  },
  {
    name: 'grep',
    kind: 'read',
    label: '搜内容',
    description: '在文件内容里搜索文字或正则，返回 "文件:行号:内容"。',
    parameters: {
      type: 'object',
      properties: {
        pattern: { type: 'string', description: '要搜的正则（或文字）' },
        path: { type: 'string', description: '在哪个文件或目录里搜，默认工作目录' },
        include: { type: 'string', description: '只搜这些文件，比如 "*.js"' },
        literal: { type: 'boolean', description: '把 pattern 当普通文字而不是正则' },
        ignoreCase: { type: 'boolean', description: '忽略大小写' },
      },
      required: ['pattern'],
    },
    summary: (a) => `${a.pattern}${a.path ? ` 在 ${a.path}` : ''}`,
    async run(a, ctx) {
      const target = resolveP(a.path, ctx)
      let r
      if (hasRg) {
        const args = ['-n', '--no-heading', '--color', 'never', '-M', '400', '-m', '50']
        if (a.literal) args.push('-F')
        if (a.ignoreCase) args.push('-i')
        if (a.include) args.push('-g', a.include)
        args.push('--', a.pattern, target)
        r = await run('rg', args, { signal: ctx.signal, timeout: 60000 })
      } else {
        const args = ['-rnI', '--exclude-dir=node_modules', '--exclude-dir=.git', '-m', '50']
        args.push(a.literal ? '-F' : '-E')
        if (a.ignoreCase) args.push('-i')
        if (a.include) args.push(`--include=${a.include}`)
        args.push('--', a.pattern, target)
        r = await run('grep', args, { signal: ctx.signal, timeout: 60000 })
      }
      const lines = r.out.split('\n').filter(Boolean)
      if (!lines.length) return '没搜到'
      return clip(lines.slice(0, 200).join('\n'), 30000) + (lines.length > 200 ? `\n……共 ${lines.length} 行，只显示前 200 行` : '')
    },
  },
  {
    name: 'sysinfo',
    kind: 'read',
    label: '看看身体',
    description: '查看服务器状况：CPU 负载、内存、硬盘、运行时间、最占资源的进程。',
    parameters: { type: 'object', properties: {} },
    summary: () => 'CPU / 内存 / 硬盘',
    async run(a, ctx) {
      const mem = (n) => `${(n / 1073741824).toFixed(2)} GB`
      const df = await run('df', ['-h', '/'], { timeout: 10000 })
      const ps = await run('bash', ['-c', 'ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head -8'], { timeout: 10000 })
      return [
        `主机名：${os.hostname()}，系统：${os.type()} ${os.release()}`,
        `已运行：${(os.uptime() / 3600).toFixed(1)} 小时`,
        `CPU：${os.cpus().length} 核，负载 ${os.loadavg().map((x) => x.toFixed(2)).join(' / ')}（1/5/15 分钟）`,
        `内存：用了 ${mem(os.totalmem() - os.freemem())} / 共 ${mem(os.totalmem())}`,
        `硬盘：\n${df.out.trim()}`,
        `最忙的进程：\n${ps.out.trim()}`,
      ].join('\n')
    },
  },
]
