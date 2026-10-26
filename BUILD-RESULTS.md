# Initial integration checks — 2026-10-26

Current hardware scope: **ESP32-S3 N4R2 and N16R8**, using the Arduino
framework/IDE. These are ESP32-S3 variants, not general Arduino-board support.
The operator reports testing both variants and a successful Arduino IDE
workflow after adding Python3 to PATH. See [ARDUINO-CLI.md](ARDUINO-CLI.md) for
the later ZIP build checks and current limits; the initial measurements below
describe their original component-build artifacts.

Scope: a new Arduino-framework library/component and separate static-webpage
example. Initial integration left Ghost Node and the ping app unchanged.
The operator subsequently flashed the N16R8; the hardware check below reads
that running firmware. No agent flashing, remote, publication or email.

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

## Separate N16R8 profile — 2026-10-26

Operator flashed the N4R2-profile image onto another ESP32-S3 identified as
N16R8 and reported repeated software resets shortly after bootloader entry.
The excerpt supplies no exception/panic cause. The existing quad-PSRAM profile
is a confirmed mismatch for Espressif's standard WROOM-1 N16R8, whose 8 MB
PSRAM is octal. This is a likely explanation, not a proven fault diagnosis.

Added `scripts/build.sh n16r8`, `sdkconfig.defaults.n16r8`, separate ignored
`sdkconfig.n16r8` and `build-n16r8/`. Only Wi-Fi settings are copied locally on
first use; configuration mode remains 0600. N4R2 keeps its original config
and build directory. The 3 MiB app partition and NVS offsets are unchanged.

Actual generated config verified: flash16MB, PSRAM_MODE_OCT,80MHz, loop32768B.
Nix full link, partition check, required server/Arduino/SecretStream symbols and
TLS0 pass. Image **1,160,768 bytes**, app partition free **1,984,960 bytes**;
SHA256 `a3f7de40fd57db9b7227ea4d20c621ff939dc6180e1798f913e402ecf3cbc844`.
This locally configured firmware must not be published or shared as a generic
binary. The previous prototype measurements above remain their own snapshot.

A real IDF reconfigure test with a copied N16R8 sdkconfig and selected N4R2
board fails with the expected memory-profile mismatch. Shell syntax check
passes.
The default N4R2 build was then rerun successfully with its original quad
configuration, required symbols and TLS0, confirming the two build paths.

No runtime library or sketch code changed for the profile correction.
The earlier quad-PSRAM QEMU startup is not N16R8 runtime evidence; the separate
hardware check below supplies that evidence. No device was reset/flashed by
the agent.

Source review also confirms Ghost Node reads its persisted `ghost-node/seed`
and generates only when missing. Its public key survives normal power cycles.
This Arduino example independently persists `p2p-arduino/seed`.

## N16R8 hardware check — 2026-10-26

Moving from the board's USB-to-UART connector to its native USB connector
exposed the application console. ROM boot messages on the UART bridge followed
by silence had not established an application stall. Firmware sends ESP-IDF
logs and Arduino `Serial` through native hardware USB Serial/JTAG.

A read-only JTAG snapshot identified `arduino-p2p` 0.1.0. The application
descriptor ELF hash exactly matched `build-n16r8/arduino-p2p.elf`:
`a19c06229b4e408ee2a960fae01504014be60e9516181875466c4e2f40b1c987`.
The matching-ELF backtrace showed the Arduino task in `loop()` at `delay(10)`;
the idle task was waiting for an interrupt. Server metadata was published.
The debugger resumed the CPU and shut down without reset or flash writes.

Native serial output then reported:

```text
ready=1 PSRAM used=29816/8388608 internal free=227043
```

This is one idle snapshot: about 29.1 KiB PSRAM, 0.36% of the available 8 MiB.
The operator subsequently confirmed webpage loading. There is no timed
comparison with Ghost Node, and this does not measure peak memory under load.

## Remaining verification

Reconnect/soak, concurrent-client load and peak internal/PSRAM measurements
remain unverified for this Arduino build. The N4R2 Arduino profile has build
and emulator evidence only. Arduino IDE/Library Manager and Axiometa Studio
packaging are outside this ESP-IDF-component build.
