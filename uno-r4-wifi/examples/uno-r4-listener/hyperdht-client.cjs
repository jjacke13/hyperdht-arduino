// SPDX-License-Identifier: LGPL-3.0-only
// Install the dependency with: npm install --no-save hyperdht@6.29.1
const assert = require('node:assert/strict')
const DHT = require('hyperdht')

async function main () {
  const [key, ...extra] = process.argv.slice(2)
  if (!/^[0-9a-f]{64}$/i.test(key || '') || extra.length) {
    throw new Error('Usage: node hyperdht-client.cjs PUBLIC_KEY')
  }
  const dht = new DHT({ host: '0.0.0.0', port: [0, 0], connectionKeepAlive: false })
  const deadline = setTimeout(() => dht.destroy({ force: true }).catch(() => {}), 45000)
  try {
    await dht.ready()
    const stream = dht.connect(Buffer.from(key, 'hex'))
    const payload = Buffer.from(Array.from({ length: 1024 }, (_, index) => index & 255))
    stream.end(payload)
    const chunks = []
    let size = 0
    for await (const chunk of stream) {
      size += chunk.length
      if (size > payload.length) throw new Error('Peer sent an oversized echo')
      chunks.push(chunk)
    }
    assert.deepEqual(Buffer.concat(chunks), payload)
    console.log('Authenticated 1 KiB echo and EOF verified')
  } finally {
    clearTimeout(deadline)
    await dht.destroy({ force: true })
  }
}

main().catch(error => {
  console.error(error.message)
  process.exitCode = 1
})
