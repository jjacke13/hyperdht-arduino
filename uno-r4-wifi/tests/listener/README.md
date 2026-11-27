# Listener feasibility tests

The fixture binds an ephemeral loopback UDP port and serves one echo. It
accepts a direct PEER_HANDSHAKE addressed to its public key, runs Noise IK
as the responder, and initializes the existing UDX and SecretStream layers
in responder mode. The fixture uses the connector's established stream
functions for UDX and record handling. It caches one handshake response,
bound to the request's Noise message and source endpoint.

`direct.cjs` supplies the loopback endpoint as a discovery result. The
stock JavaScript client still sends and verifies the real handshake and
exchanges the encrypted record and EOF. This checks the reachable direct
path.

`publish.cjs` starts a stock JavaScript test DHT. The fixture starts from
its ephemeral bootstrap, walks the DHT and publishes to the three closest
persistent nodes. The test checks the announced set by XOR distance.
It repeats signed publication, then a stock client discovers the listener by
key and completes a relayed handshake and direct encrypted data exchanges.
Two successive connections use the same key. The stock client tests 1-byte,
sensor-status JSON and 1,024-byte records on each connection.
The fixture signs UNANNOUNCE after the second session closes. A fresh stock client confirms removal
with a lookup and a query to every persistent node.

For the publication test, the fixture applies endpoint filtering in its
receive loop: rendezvous traffic is admitted, and direct peer datagrams
are dropped until the listener sends to that exact endpoint. The
firewalled stock client must reach the echo through an authenticated
punch, with at least one pre-punch datagram dropped. Setting
`HDT_TEST_OPEN_CLIENT=1` checks the stock open-client path against the
same receiver. The LAN variant runs the same test inside an isolated
namespace with a private interface and requires two sessions on the
stock local ping and direct UDX path. Board runtime and additional NAT mapping behavior require
separate checks.

The handshake roles and direct RPC framing follow hyperdht-cpp
`35514620b5e050817fe15255333f6ab4427f469c`, primarily
`src/server_connection.cpp` and `src/router.cpp`. The test reuses Tiny's
already attributed Noise, UDX and record implementation.

## Run

From the repo root with a C compiler, pkg-config, host libsodium, Node.js,
and Linux user/network namespaces:

```sh
out=$(mktemp -d)
cc -std=c99 -Wall -Wextra -Werror -g -O1 \
  -fsanitize=address,undefined -fno-pie -no-pie -Iinclude -Isrc \
  $(pkg-config --cflags libsodium) tests/listener/direct.c \
  src/{connector,handshake,listener,listener_punch,noise,publish,publish_walk,record,rpc,transport,udx_rx}.c \
  $(pkg-config --libs libsodium) -o "$out/listener"
npm install --prefix "$out" --no-save hyperdht@6.29.1
NODE_PATH="$out/node_modules" timeout 20 unshare -Urn bash -c \
  'ip link set lo up && node "$@"' _ tests/listener/direct.cjs "$out/listener"
NODE_PATH="$out/node_modules" timeout 20 unshare -Urn bash -c \
  'ip link set lo up && node "$@"' _ tests/listener/publish.cjs "$out/listener"
```

To verify the LAN shortcut, add a private interface inside the namespace:

```sh
HDT_TEST_LAN=1 NODE_PATH="$out/node_modules" timeout 25 unshare -Urn bash -c '
  set -e
  ip link set lo up
  ip link add bench0 type dummy
  ip addr add 192.168.254.1/24 dev bench0
  ip link set bench0 up
  node "$@"
' _ tests/listener/publish.cjs "$out/listener"
```

Each test prints a success line and exits zero.

To exercise the remote client file, pass
`examples/uno-r4-listener/hyperdht-client.cjs` as the last argument to
`publish.cjs`. The test supplies its private bootstrap list. The client file
receives only the public key and runs its regular CLI entry point.

## Persistent settings

From the repo root, with a C++ compiler and host libsodium:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -g -O1 \
  -fsanitize=address,undefined -fno-pie -no-pie \
  $(pkg-config --cflags libsodium) tests/listener/settings.cpp \
  $(pkg-config --libs libsodium) -o /tmp/hdtiny-test-settings
/tmp/hdtiny-test-settings
```

The test uses the firmware's settings code with a simulated flash device.
It checks first provisioning, repeat boot reads, a torn write, preservation
of the identity when Wi-Fi credentials change, and read-failure behavior.
