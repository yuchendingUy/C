// 服务器 → 浏览器 的实时通道（SSE）
const clients = new Set()

export function addClient(res) {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream; charset=utf-8',
    'Cache-Control': 'no-cache, no-transform',
    Connection: 'keep-alive',
    'X-Accel-Buffering': 'no',
  })
  res.write('retry: 3000\n\n')
  clients.add(res)
  const ping = setInterval(() => res.write(': ping\n\n'), 20000)
  res.on('close', () => {
    clearInterval(ping)
    clients.delete(res)
  })
}

export function broadcast(event) {
  const data = `data: ${JSON.stringify(event)}\n\n`
  for (const res of clients) res.write(data)
}

export const onlineCount = () => clients.size
