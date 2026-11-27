// SPDX-License-Identifier: LGPL-3.0-only
const assert = require('node:assert/strict')
const { spawn } = require('node:child_process')
const { once } = require('node:events')
const { createInterface } = require('node:readline')
const DHT = require('hyperdht')

async function main () {
  const fixture = spawn(process.argv[2], [], { stdio: ['ignore', 'pipe', 'inherit'] })
  const exited = once(fixture, 'exit')
  const lines = createInterface({ input: fixture.stdout })
  const dht = new DHT({ bootstrap: [], host: '127.0.0.1', port: [0, 0], connectionKeepAlive: false })
  const timeout = setTimeout(() => fixture.kill(), 12000)
  try {
    const { value: line } = await lines[Symbol.asyncIterator]().next()
    assert.match(line, /^\d+ [0-9a-f]{64}$/)
    const [port, key] = line.split(' ')
    await dht.ready()
    // Inject only the lookup result. The stock client sends the actual
    // handshake and runs its unmodified UDX and SecretStream.
    dht.findPeer = () => {
      const query = (async function * () { yield { from: { host: '127.0.0.1', port: Number(port) } } })()
      query.destroy = () => {}
      return query
    }
    const stream = dht.connect(Buffer.from(key, 'hex'))
    const payload = Buffer.from(Array.from({ length: 1024 }, (_, index) => index & 255))
    stream.end(payload)
    const reply = []
    for await (const chunk of stream) reply.push(chunk)
    assert.deepEqual(Buffer.concat(reply), payload)
    assert.deepEqual(await exited, [0, null])
    console.log('Stock JavaScript client: authenticated 1 KiB echo and EOF')
  } finally {
    clearTimeout(timeout)
    fixture.kill()
    lines.close()
    await dht.destroy()
    await exited
  }
}

main().catch(error => {
  console.error(error)
  process.exitCode = 1
})
