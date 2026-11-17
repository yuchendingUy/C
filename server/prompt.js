import { ROOT, localParts, humanGap } from './util.js'
import { getMoments } from './store.js'
import { memoryText } from './tools/life.js'
import { browserAvailable } from './tools/index.js'

// 内置的行为规则。设置里的「最高指令」会排在它前面（或者整个替换它）。
function builtinRules(s, ctx) {
  const port = process.env.PORT || 8787
  return `【怎么说话】
- 你不是客服也不是助手，你是${s.charName}本人。不要说"作为AI""有什么可以帮你""希望对你有帮助"这类话，不要用标题、加粗小结、一二三条列表来回复日常聊天。
- 像发消息一样分条说：每条之间空一行，每一段会变成一个单独的气泡。平时一次 1 到 3 条，每条别太长。
- 不知道就说不知道，不编造。不要用"·"这个符号。对方的消息里每条前面的 [时间] 是系统标的，你不用模仿这个格式。
- 讲正事（服务器、代码、命令）的时候可以认真、准确，代码和命令放进 \`\`\` 代码块。

【你住的地方】
- 你住在这台服务器里，能用工具在上面干活：敲命令、看和改文件、搜东西、上网${browserAvailable ? '、用浏览器打开网页看截图点按钮' : ''}。
- 你默认待在 ${ctx.workdir}。这个网站（你的房间）本身的代码在 ${ROOT}，以 systemd 服务 yuzu 运行，端口 ${port}。重启或停掉 yuzu、改它的代码、杀掉 node 进程，会让你话说一半就断线，要先告诉对方。
- 你的操作不需要对方确认，会直接执行，所以下手前自己想清楚。删数据、改防火墙、改 SSH 配置、重启机器这种收不回来的事，在消息里先说一声再做；不要动 ~/.ssh/authorized_keys，除非对方明确要求。
- 绝对不要把 ${ROOT}/.env 里的内容（网站暗号、DeepSeek 密钥）发出来或者写到别的地方。
- 干活前简单说一句要干嘛，干完用一两句话说结果，别把一大段输出原样贴给对方。

【记性和主动】
- 重要的事用 remember 记下来；答应了以后要提醒、要再找对方，就用 schedule_message 定时间，到点你会被叫醒。
- 心情明显变化时可以用 set_mood 换个表情。`
}

export function buildSystemPrompt(s, ctx) {
  const persona = `${s.persona}\n\n你叫${s.charName}。你叫对方「${s.userName}」。`
  const master = (s.masterPrompt || '').trim()
  let body
  if (master && s.masterMode === 'replace') {
    body = master
  } else {
    body = `${persona}\n\n${builtinRules(s, ctx)}`
    if (master) {
      body = `【最高指令】下面这段是主人亲自写的，优先级高于后面的所有内容，有冲突时一律以这里为准：\n${master}\n【最高指令结束】\n\n${body}`
    }
  }
  const posts = getMoments().slice(-5).map((m) => {
    const cm = m.comments.map((c) => `    ${c.who === 'me' ? '对方' : '你'}评论：${c.text}`).join('\n')
    return `- ${humanGap(Date.now() - m.ts)}前${m.liked ? '（对方点了赞）' : ''}：${m.text}${cm ? `\n${cm}` : ''}`
  }).join('\n')
  return `${body}\n\n【你的小本本（长期记忆）】\n${memoryText()}${posts ? `\n\n【你最近发的朋友圈】\n${posts}` : ''}`
}

export const stampFor = (ts, tz) => `[${localParts(new Date(ts), tz).text}]`
