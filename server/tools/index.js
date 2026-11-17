import { systemTools } from './system.js'
import { webTools } from './web.js'
import { browserTools, browserAvailable } from './browser.js'
import { lifeTools, momentTools } from './life.js'

export { browserAvailable }
export const ALL_TOOLS = [...systemTools, ...webTools, ...browserTools, ...lifeTools, ...momentTools]
const byName = new Map(ALL_TOOLS.map((t) => [t.name, t]))
export const getTool = (name) => byName.get(name)

// 转成 DeepSeek 的 tools 格式：{type:'function', function:{name, description, parameters}}
export const apiTools = () => ALL_TOOLS.map((t) => ({
  type: 'function',
  function: { name: t.name, description: t.description, parameters: t.parameters },
}))

export const toolMeta = () => Object.fromEntries(ALL_TOOLS.map((t) => [t.name, { label: t.label, kind: t.kind }]))
