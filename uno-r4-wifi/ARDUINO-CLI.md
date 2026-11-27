# HyperDHT Tiny for Arduino Uno R4 WiFi

The library runs on the Renesas RA4M1 and uses the board's ESP32-S3 Wi-Fi
bridge firmware 0.6.0. This preview targets Arduino Renesas UNO 1.6.0 and
supports one role per sketch. Use the client for an outbound IPv4 session
or the server for one active inbound session at a time. Each SecretStream
plaintext record must be at most 1 KiB.

## Install and compile

Install the generated ZIP with Arduino CLI's `lib install --zip-path`.
For a local ZIP, enable `library.enable_unsafe_install` in the intended CLI
configuration. Use the included build helper with Bash, Python 3 and patch.
It creates private copies of the core's linker variant and WiFiS3 library for
the 8 KiB stack and the bounded UDP receive path.

```sh
arduino-cli lib install --zip-path /path/to/HyperDHT_Tiny_R4-0.1.0.zip
library=/path/to/sketchbook/libraries/HyperDHT_Tiny_R4
cp -R "$library/examples/HyperDHTEcho" /path/to/sketches/
bash "$library/extras/compile-r4.sh" /path/to/sketches/HyperDHTEcho
cp -R "$library/examples/HyperDHTListenEcho" /path/to/sketches/
bash "$library/extras/compile-r4.sh" /path/to/sketches/HyperDHTListenEcho
```

The same helper compiles your own sketch directory. The ELF, map and binary
are in the printed build directory. The helper performs no upload. If the
Arduino CLI needs an explicit Arm toolchain location, set
`HDT_R4_TOOLCHAIN` to the GCC 7-2017q4 directory. `ARDUINO_CLI` and `PYTHON`
can point to specific installed executables.

Fill in the Wi-Fi placeholders in your local copy of either sketch. With
`hyperdht@6.29.1` installed on your Node.js host:

- For `HyperDHTEcho`, run `node extras/hyperdht-echo.cjs`. Paste the host
  server's 64-character key into the board's serial terminal at 115200.
- For `HyperDHTListenEcho`, read `PUBLIC_KEY=` on the serial terminal and
  run `node extras/hyperdht-client.cjs PUBLIC_KEY`. Send `q` over serial to
  withdraw the announcement and stop listening.

## Client API

Use `#include <HyperDHTClient.h>`. Join Wi-Fi before calling
`HyperDHTClient::begin(key)` with the 32 raw bytes of the remote public key.
Only use the API from the sketch's `setup()` and `loop()`. The client owns one
socket and one session, uses a fresh client identity per connection, and
tries direct, eligible LAN, and bounded hole-punch paths.

- Call `poll()` frequently. The SDK's DNS and Wi-Fi modem operations are
  synchronous. Wait for `Connected` before writing.
- `write(data, size)` queues one 1–1024 byte record. A false return means
  the write was rejected, for example because the previous write is pending.
- `read(size)` borrows authenticated plaintext, or returns null. Call
  `release()` after consuming it. Polling pauses while the view is borrowed.
- `RemoteEnd` indicates peer EOF. `end()` queues local EOF when it can;
  keep polling until `Finished` or `Failed`.
- `close()` aborts the session and releases its socket while keeping Wi-Fi
  connected. If a truncated modem reply leaves the UART out of sync, reset
  the board before starting a new session.

## Server API

Include `HyperDHTServer.h` and join Wi-Fi before calling
`HyperDHTServer::begin()`. The server exposes `publicKey()` for its 32-byte
key. Supply a private 32-byte seed to `begin(seed)` to reuse an identity.
The sketch owns Wi-Fi and seed storage. The source repository's standalone
sketch under `examples/uno-r4-listener` includes persistent settings and
the LED demo.

- Call `poll()` frequently to advance discovery, announcements, their refresh
  and stream I/O. The adapter does no busy waiting. Underlying DNS and Wi-Fi
  modem commands can block per call.
- `read(size)` returns a view valid until the next `poll()`, `release()`
  or `stop()`. Use `write(data, size)` for one 1–1024 byte record. A false
  return can mean the last record is still pending. Keep the input record
  and acquire the view again after polling.
- On `RemoteEnd`, finish pending reads, call `end()` to queue your EOF and
  keep polling. After closure the server returns to `Listening`.
- `stop()` releases the peer session and starts withdrawal. Keep polling
  until `Idle` or `Failed`. A partial modem reply requires a board reset.

The listener uses public-key discovery, the eligible LAN path and bounded
direct hole punching. Keep client and server in separate firmware builds.
This package has no combined-role memory budget.

## Build the ZIP from source

Follow the pinned Arm GCC and hard-float libsodium 1.0.20 build in
`docs/R4-SDK-LINK.md`. Keep a separate unbuilt copy of the corresponding
libsodium source to include in the package. When rebuilding from the
ZIP, the bundled libsodium archive supplies that source. Then run:

```sh
bash scripts/package-arduino.sh "$toolchain" "$sodium_build" "$sodium_source"
```

This generates an installer under `build-r4-sdk/package.*`. The ZIP includes
the corresponding Tiny and libsodium sources, rebuild instructions and
license notices.
