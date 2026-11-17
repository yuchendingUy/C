// 让柚柚"像个人"的工具：记事、约时间主动找你、换心情
import { addMemory, removeMemory, getMemory, addSchedule, getSchedules, updateSchedules, patchState } from '../store.js'
import { zonedToDate, localParts } from '../util.js'

export const MOODS = {
  happy: '开心', shy: '害羞', sleepy: '困', pouty: '鼓着脸', excited: '兴奋', worried: '担心', focused: '认真', sad: '难过',
}

export const lifeTools = [
  {
    name: 'remember',
    kind: 'safe',
    label: '记在小本本上',
    description: '把关于对方的重要事情记进长期记忆（喜好、生日、在忙的事、你们的约定、对方说过要你记住的事）。记忆会一直带在你身边，即使聊天记录被压缩也不会忘。不要记琐碎的事。',
    parameters: { type: 'object', properties: { text: { type: 'string', description: '一句话写清楚要记的事' } }, required: ['text'] },
    summary: (a) => a.text,
    async run(a) {
      const m = await addMemory(a.text)
      return `记好了（编号 ${m.id}）`
    },
  },
  {
    name: 'forget',
    kind: 'safe',
    label: '划掉一条',
    description: '删掉一条已经过时或错误的长期记忆。',
    parameters: { type: 'object', properties: { id: { type: 'string', description: '记忆编号' } }, required: ['id'] },
    summary: (a) => a.id,
    async run(a) {
      return (await removeMemory(a.id)) ? '划掉了' : '没找到这条'
    },
  },
  {
    name: 'schedule_message',
    kind: 'safe',
    label: '定个闹钟',
    description: `约一个时间，到点后你会被叫醒并主动给对方发消息。用在：答应了要提醒对方、约好之后再聊、想过一阵关心一下对方的事（比如对方说明天考试，就约在考完后问问）。
时间用 at（对方所在时区的 "YYYY-MM-DD HH:MM"）或 in_minutes 二选一。repeat="daily" 表示每天这个时间都来。`,
    parameters: {
      type: 'object',
      properties: {
        at: { type: 'string', description: '"YYYY-MM-DD HH:MM"，对方当地时间' },
        in_minutes: { type: 'number', description: '多少分钟之后' },
        note: { type: 'string', description: '到时候要干嘛，写给未来的自己看' },
        repeat: { type: 'string', enum: ['none', 'daily'] },
      },
      required: ['note'],
    },
    summary: (a) => `${a.at || `${a.in_minutes} 分钟后`}：${a.note}`,
    async run(a, ctx) {
      const tz = ctx.settings.timezone
      let when = null
      if (a.in_minutes) when = new Date(Date.now() + Number(a.in_minutes) * 60000)
      else if (a.at) when = zonedToDate(a.at, tz)
      if (!when || isNaN(when)) return '错误：时间格式不对，用 "YYYY-MM-DD HH:MM" 或 in_minutes'
      if (when.getTime() < Date.now() - 60000) return '错误：这个时间已经过去了'
      const s = await addSchedule({ at: when.getTime(), note: String(a.note), repeat: a.repeat === 'daily' ? 'daily' : 'none', convId: ctx.conv.id })
      return `定好了（编号 ${s.id}）：${localParts(when, tz).text}${s.repeat === 'daily' ? '，每天重复' : ''}`
    },
  },
  {
    name: 'list_schedules',
    kind: 'safe',
    label: '翻翻日程',
    description: '看看已经约好的时间。',
    parameters: { type: 'object', properties: {} },
    summary: () => '日程表',
    async run(a, ctx) {
      const list = getSchedules()
      if (!list.length) return '现在没有约好的事'
      return list.map((s) => `${s.id}  ${localParts(new Date(s.at), ctx.settings.timezone).text}${s.repeat === 'daily' ? '（每天）' : ''}  ${s.note}`).join('\n')
    },
  },
  {
    name: 'cancel_schedule',
    kind: 'safe',
    label: '取消约定',
    description: '取消一个约好的时间。',
    parameters: { type: 'object', properties: { id: { type: 'string' } }, required: ['id'] },
    summary: (a) => a.id,
    async run(a) {
      let hit = false
      await updateSchedules((l) => l.filter((s) => (s.id === a.id ? !(hit = true) : true)))
      return hit ? '取消了' : '没找到'
    },
  },
  {
    name: 'set_mood',
    kind: 'safe',
    label: '换个表情',
    description: `改变你现在的心情，房间里你的表情会跟着变。心情变化明显时用一下就好，不用每句都调。可选：${Object.entries(MOODS).map(([k, v]) => `${k}(${v})`).join('、')}`,
    parameters: { type: 'object', properties: { mood: { type: 'string', enum: Object.keys(MOODS) } }, required: ['mood'] },
    summary: (a) => MOODS[a.mood] || a.mood,
    async run(a, ctx) {
      if (!MOODS[a.mood]) return '没有这种心情'
      await patchState({ mood: a.mood })
      ctx.emit({ type: 'mood', mood: a.mood })
      return `现在的心情：${MOODS[a.mood]}`
    },
  },
]

export function memoryText() {
  const m = getMemory()
  if (!m.length) return '（还没有记下什么）'
  return m.map((x) => `- [${x.id}] ${x.text}`).join('\n')
}
