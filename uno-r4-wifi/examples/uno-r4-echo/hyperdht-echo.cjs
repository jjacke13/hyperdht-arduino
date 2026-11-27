// Run with Node.js after: npm install --no-save hyperdht@6.29.1
const DHT = require('hyperdht')

const size = 1024

async function startEcho(dht) {
  let active = false
  const server = dht.createServer((socket) => {
    socket.on('error', (error) => console.error('Stream error:', error.message))
    if (active) {
      socket.destroy()
      return
    }
    active = true
    const payload = Buffer.alloc(size)
    let received = 0
    const timeout = setTimeout(() => socket.destroy(new Error('Echo deadline exceeded')), 15000)
    socket.once('close', () => {
      clearTimeout(timeout)
      active = false
    })
    socket.on('data', (chunk) => {
      if (chunk.length > size - received) {
        socket.destroy(new Error('Expected exactly 1024 bytes'))
        return
      }
      Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength).copy(payload, received)
      received += chunk.length
      if (received === size) {
        socket.end(payload)
        console.log('Echoed 1024 bytes')
      }
    })
    socket.once('end', () => {
      if (received !== size) socket.destroy(new Error('Incomplete echo request'))
    })
  })
  await server.listen()
  return server
}

async function main() {
  const port = Number(process.argv[2] ?? 0)
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error('Invalid UDP port')

  // A zero-only port range requests an OS-assigned port in HyperDHT 6.29.
  const dht = new DHT({ host: '0.0.0.0', port: port || [0, 0], connectionKeepAlive: false })
  const stop = () => dht.destroy().catch((error) => {
    console.error(error.message)
    process.exitCode = 1
  })
  process.once('SIGINT', stop)
  process.once('SIGTERM', stop)

  try {
    console.log('Joining HyperDHT...')
    const server = await startEcho(dht)
    if (dht.destroyed) return
    console.log('UDP_PORT=' + dht.address().port)
    console.log('REACHABILITY=' + (dht.remoteAddress() ? 'direct' : 'unconfirmed'))
    console.log('PUBLIC_KEY=' + server.address().publicKey.toString('hex'))
  } catch (error) {
    await dht.destroy({ force: true })
    throw error
  }
}

module.exports = startEcho
if (require.main === module) main().catch((error) => {
  console.error(error.message)
  process.exitCode = 1
})
