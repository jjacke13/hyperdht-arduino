// SPDX-License-Identifier: LGPL-3.0-only
const assert = require('node:assert/strict')
const { spawn, execFile } = require('node:child_process')
const { promisify } = require('node:util')
const { resolve } = require('node:path')
const { once } = require('node:events')
const { createInterface } = require('node:readline')
const testnet = require('hyperdht/testnet')
const sodium = require('sodium-universal')

async function main () {
  const openClient = process.env.HDT_TEST_OPEN_CLIENT === '1'
  const lan = process.env.HDT_TEST_LAN === '1'
  const net = await testnet(6, { host: lan ? '192.168.254.1' : '127.0.0.1' })
  const storageNodes = net.nodes.slice(1)
  const persistent = storageNodes.map(node => node.address())
  const args = [String(net.bootstrap[0].port)]
  if (lan) args.push('lan')
  const fixture = spawn(process.argv[2], args, { stdio: ['ignore', 'pipe', 'inherit'] })
  const exited = once(fixture, 'exit')
  const lines = createInterface({ input: fixture.stdout })
  const messages = lines[Symbol.asyncIterator]()
  const timeout = setTimeout(() => fixture.kill(), 22000)

  try {
    const { value: line } = await messages.next()
    assert.match(line, /^\d+ [0-9a-f]{64}$/)
    const [, key] = line.split(' ')
    const publicKey = Buffer.from(key, 'hex')
    assert.equal((await messages.next()).value, 'ANNOUNCED')
    assert.equal((await messages.next()).value, 'REFRESHED')

    const dht = net.createNode({
      host: lan ? '0.0.0.0' : '127.0.0.1',
      connectionKeepAlive: false,
      firewalled: !process.argv[3] && !openClient,
      quickFirewall: false
    })
    await dht.fullyBootstrapped()

    const found = await find(dht, publicKey)
    assert(found.some(({ from, peer }) => persistent.some(node => node.port === from.port) &&
      peer.publicKey.equals(publicKey)))
    const target = Buffer.alloc(32)
    sodium.crypto_generichash(target, publicKey)
    const observed = await Promise.all(persistent.map(node => dht.request({ command: 2, target }, node)))
    const closest = storageNodes.map(node => {
      const distance = Buffer.from(node.id.map((byte, i) => byte ^ target[i]))
      return { distance, port: node.address().port }
    }).sort((a, b) => Buffer.compare(a.distance, b.distance)).slice(0, 3)
    const published = observed.filter(reply => reply.value).map(reply => reply.from.port)
    assert.deepEqual(published.sort(), closest.map(node => node.port).sort())

    for (const session of [0, 1]) {
      const external = session === 0 ? process.argv[3] : undefined
      if (external) {
        // Override only the stock bootstrap list in the child.
        const child = `
          const [client, bootstrap, ...args] = process.argv.slice(1)
          global.Pear = { config: { dht: { bootstrap: JSON.parse(bootstrap) } } }
          process.argv = [process.execPath, client, ...args]
          require(client)
        `
        const { stdout } = await promisify(execFile)(process.execPath, [
          '-e', child, resolve(external), JSON.stringify(net.bootstrap), key
        ], { timeout: 10000 })
        assert.match(stdout, /Authenticated 1 KiB echo and EOF verified/)
      } else {
        await echoRecords(dht, publicKey)
      }
      const { value: direct } = await messages.next()
      assert.match(direct, new RegExp(`^DIRECT probe=[01] open=[01] blocked=\\d+ records=${external ? 1 : 3} lan=${Number(lan)}$`))
      if (lan) {
        assert.match(direct, /^DIRECT probe=0 open=0 /)
        assert(!dht.remoteAddress())
      } else if (openClient) {
        assert.match(direct, /^DIRECT probe=0 open=1 /)
        assert(dht.remoteAddress())
      } else if (!process.argv[3]) {
        assert.match(direct, /^DIRECT probe=1 open=0 blocked=[1-9]\d* /)
        assert(!dht.remoteAddress())
      }
      console.log(direct)
    }
    assert.equal((await messages.next()).value, 'WITHDRAWN')
    assert.deepEqual(await exited, [0, null])
    const fresh = net.createNode()
    await fresh.fullyBootstrapped()
    const remaining = await find(fresh, publicKey)
    assert(!remaining.some(({ peer }) => peer.publicKey.equals(publicKey)))
    const removed = await Promise.all(persistent.map(node => fresh.request({ command: 2, target }, node)))
    assert(removed.every(reply => !reply.value))
    console.log('One key served two successive stock clients and withdrew cleanly')
  } finally {
    clearTimeout(timeout)
    fixture.kill()
    await exited
    lines.close()
    await net.destroy()
  }
}

async function echoRecords (dht, publicKey) {
  const stream = dht.connect(publicKey)
  const records = [
    Buffer.from([0x5a]),
    Buffer.from('{"temperature_c":23.4,"humidity_pct":51,"motion":false}'),
    Buffer.from(Array.from({ length: 1024 }, (_, index) => index & 255))
  ]
  const incoming = stream[Symbol.asyncIterator]()
  try {
    for (const record of records) {
      stream.write(record)
      const data = []
      let size = 0
      while (size < record.length) {
        const { value, done } = await incoming.next()
        assert(!done, 'Unexpected EOF')
        size += value.length
        assert(size <= record.length, 'Oversized echo')
        data.push(value)
      }
      assert.deepEqual(Buffer.concat(data), record)
    }
    stream.end()
    assert((await incoming.next()).done)
  } finally {
    stream.destroy()
  }
}

async function find (dht, key) {
  const results = []
  for await (const result of dht.findPeer(key)) results.push(result)
  return results
}

main().catch(error => {
  console.error(error)
  process.exitCode = 1
})
