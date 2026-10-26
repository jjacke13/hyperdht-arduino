# Initial integration checks — 2026-10-26

Scope: a new Arduino-framework library/component and separate static-webpage
example. No Ghost Node or ping-app files changed; both existing repos checked
clean. No physical hardware was flashed. No remote, publication or email.

## Firmware cross-build

Nix `hyperdht-demo#esp32`, ESP-IDF 5.5.2, Xtensa GCC 14.2; managed
Arduino-ESP32 3.3.6, libsodium 1.0.21, committed transitive dependency lock.
Public placeholder Wi-Fi SSID `YOUR_WIFI_SSID`, empty password. This retains
the full server code in the build and is not a configured deployment image.

- Complete ESP32-S3 ELF and firmware image linked successfully.
- Image: **1,157,184 bytes** (about 1.104 MiB).
- Factory application partition: 3,145,728 bytes; free **1,988,544 bytes**, 63.2%.
- SHA-256: `5fe0f3ca64f51e7d7329ebc21b67bfbea6947f485045834a5a3f29c64a02d422`.
- GCC TLS linker span: **0 bytes per task**; build script enforces this.
- Required Arduino setup/loop task, Holesail begin, server listen, mutable put
  and SecretStream receive symbols verified in the actual ELF.
- DIRAM occupancy 111,759 bytes includes code. Data 17,176 + BSS 20,200 bytes
  are static data, not peak runtime heap. Separate IRAM region occupies 16,384
  bytes including vectors; this does not mean all chip instruction RAM is full.
- Four fixed application connection structures: **10,464 bytes** on target,
  allocated in PSRAM when available, normal-heap fallback. This is not total
  runtime PSRAM consumption: DHT/transport/framework allocations are additional.

The example uses a 32 KiB Arduino loop task, 1000 Hz FreeRTOS tick and hardware
CDC/JTAG Serial. Only Arduino WiFi, Network and Preferences libraries are
selected. The upstream manifest still downloads/builds other declared managed
dependencies; compiled archive size does not equal linked firmware size.
Arduino selects SO_RCVBUF support, unlike the earlier Ghost Node config, so
runtime UDP queue behavior and memory must be measured on hardware.

Arduino's logging wrapper symbols exist in its archive but were not extracted
for late Wi-Fi references. Two explicit undefined-symbol link flags resolve
that static archive wiring. No dependency source patch was needed for Arduino.
Sketch compilation explicitly declares Wi-Fi, NVS, netif/event and managed
network-provisioning header dependencies. An initial commit in this independent
repo avoids managed RainMaker/Insights Git-version probes traversing into the
Desktop/repos parent or failing on an unborn HEAD.

## Host checks

`nix-shell tests/shell.nix --run tests/run.sh`:

- ASan/UBSan HTTP parser: every request split, HEAD/method/route behavior,
  framing, duplicate lengths, control bytes, pipelining and header bounds.
- Real wrapper code with mock C API/transport: startup failures, shutdown and
  restart, drain-before-free, independent object state, original Holesail JS
  URL vector, metadata sequence preservation/increment/overflow, publish retry,
  HTTP chunk/drain ordering, four slots, idle-close initiation and IP refresh.

Mocks validate wrapper behavior, not HyperDHT packet-level operation or actual
phone interoperability. Codecs are the original dependency implementation.

## Emulator startup

Official Espressif QEMU 9.2.2, ESP32-S3, 2 MiB PSRAM, fresh 4 MiB flash image.
GDB reached the real sketch's **setup()**, called from the real Arduino
**loopTask**; panic breakpoint was not reached first. This exercises scheduler,
global initialization and Arduino task startup before application setup.
GDB verified the current FreeRTOS task name and target connection-structure size.

The emulator was stopped at entry to setup, before Serial/Wi-Fi/server setup.
This proves neither Wi-Fi nor actual P2P operation. Emulator and debugger were
terminated after the check; no lab, device or running service was touched.

## Not yet verified

No flash/runtime/phone test of this Arduino example, reconnect/soak or peak
internal/PSRAM measurements. Earlier Ghost Node operator successes do not prove
this new Arduino integration. Arduino IDE/Library Manager and Axiometa Studio
packaging are outside this first ESP-IDF-component build.
