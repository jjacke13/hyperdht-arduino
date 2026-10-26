# HyperDHT for Arduino on ESP32

Write ordinary Arduino `setup()`/`loop()` application code while HyperDHT
handles encrypted P2P connections. The first high-level API is a public-mode
Holesail-compatible server for one static webpage:

```cpp
#include <Arduino.h>
#include <HolesailWebServer.h>

HolesailWebServer server;
static const char page[] = "<h1>Hello from ESP32</h1>";

void setup() {
    Serial.begin(115200);
    // Connect Wi-Fi first. See the complete example for WiFi.begin().
    if (!server.begin(page)) abort();
    Serial.println(server.link());
}

void loop() {
    server.poll();
    delay(10);
}
```

The complete [PublicWebServer sketch](examples/PublicWebServer/PublicWebServer.ino)
uses Arduino WiFi and Preferences and persists its identity across reboots.
Without a seed supplied to `begin`, each start generates a new identity/link.
`begin` starts asynchronous discovery; `ready()` becomes true after bootstrap,
announcement and successful signed metadata publication.

## Supported build

This initial version is an **Arduino-framework ESP-IDF component**. The actual
application uses Arduino APIs, `setup()` and `loop()`, with Espressif's Arduino
core running underneath. Build it using Nix and ESP-IDF. This is not yet an
Arduino IDE/Library Manager ZIP: copying this directory into Arduino libraries
does not install or configure the native dependencies.

Pinned integration: ESP-IDF 5.5.2, Arduino-ESP32 3.3.6, libsodium 1.0.21;
ESP32-S3 N4R2 (4 MB flash / 2 MB quad PSRAM) or standard WROOM-1 N16R8
(16 MB flash / 8 MB octal PSRAM), 3 MiB factory partition, no OTA.
The sketch has no display dependency and does not use any board-specific GPIO.
Other ESP32 variants have not been validated.

```sh
export HYPERDHT_DIR=/home/jacke/agents/alberto/work/hyperdht-demo
export HOLESAIL_DIR=/home/jacke/agents/alberto/work/holesail-esp32
nix develop "$HYPERDHT_DIR#esp32"
cd /home/jacke/Desktop/repos/hyperdht-arduino
bash scripts/build.sh
```

The first build downloads Espressif's Arduino component and its declared
dependencies. Selective compilation enables WiFi, Network and Preferences.
Configure Wi-Fi with `idf.py menuconfig` from `examples/PublicWebServer`
(P2P example), then rerun the build. Keep credentials in ignored local config;
never distribute a firmware image compiled with personal Wi-Fi credentials.
The initial build-check image used the public placeholder `YOUR_WIFI_SSID`;
configure your network before flashing. The committed dependency lock pins transitive
component versions as well as the direct dependencies.

The underlying HyperDHT checkout must be bb16c0b0fab5c4dccfde50120955ea0a1730b903
with `embedded-tls.patch` applied; UDX submodule is
759bf7641347f65a1896d4bc4c3b1606a459f466. Holesail codecs come unmodified from
f57d423b2d65a660468cbb8029536ec99b9e79c2. No owner checkout needs modification.
The TLS patch removes an embedded per-task RNG allocation that previously
overflowed the idle-task stack. It is required until an accepted upstream
replacement is pinned and tested.

### N16R8 memory profile

The N4R2 quad-PSRAM image is not a universal ESP32-S3 image. Espressif's
[WROOM-1 datasheet](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
specifies octal PSRAM for N16R8. Select its profile explicitly:

```sh
# Inside the same Nix shell, from the repository root:
bash scripts/build.sh n16r8
cd examples/PublicWebServer
idf.py -B build-n16r8 menuconfig
```

The N16R8 build uses `build-n16r8/` and ignored `sdkconfig.n16r8`, independently
of the N4R2 `build/` and `sdkconfig`. On first use only the Wi-Fi settings are
copied from the existing configuration, locally without printing them. Edit
P2P example in menuconfig if needed, then rerun `bash scripts/build.sh n16r8`
from the repository root. CMake rejects a board/config memory-mode mismatch.

Before flashing, close any monitor and verify the connected device using
`python -m esptool --chip esp32s3 --port PORT chip_id` and `flash_id`. For a
confirmed N16R8, flash from the example directory using:

```sh
idf.py -B build-n16r8 -p PORT flash monitor
```

Use the board's **native USB/OTG connector**, not a USB-to-UART connector.
Both the ESP-IDF console and Arduino `Serial` use hardware USB Serial/JTAG.
A UART bridge can show ROM boot messages and then go silent while the app runs.
For a board already flashed with the N16R8 build, monitoring alone is enough:

```sh
idf.py -B build-n16r8 -p PORT monitor
```

The sketch prints `ready=1` and its public `hs://` link every 30 seconds once
the server is ready. Exit the monitor with Ctrl+]. For N4R2, use `-B build`.

No whole-flash erase is required. This example keeps its seed in the
`p2p-arduino` NVS namespace; Ghost Node uses `ghost-node`. Normal power cycles
retain an existing seed and public link. Erasing the NVS partition loses it.

## API and limits

- `HolesailWebServer`: `begin(page, optionalSeed)`, `poll()`, `ready()`,
  `link()`, `end()`. Page data must remain valid and unchanged until `end()`.
- Four application connection slots, 2 KiB request-header limit, acknowledged
  1 KiB response chunks, 30-second idle-close initiation. PSRAM used when
  available; connection slots fall back to normal heap.
- GET/HEAD `/` only; other routes do not expose device controls. No request
  bodies, uploads, private mode, UDP forwarding or arbitrary local TCP proxy.
  Public links permit any client identity to connect. Transport encryption
  does not make the phone's local HTTP endpoint HTTPS.
- All methods belong to the same task as `setup()`/`loop()`. No library worker
  task is created. Call `poll()` frequently; avoid long blocking application
  work. `end()` drains graceful shutdown and may block for network timeouts;
  never invoke it from a HyperDHT callback. Do not call the API from Wi-Fi
  event callbacks or interrupts.
- The example configures a 32 KiB Arduino loop stack. Keep this setting when
  integrating; the default Arduino loop stack has not been validated here.
- Raw HyperDHT use: `HyperDHTNode` owns lifecycle/event-loop polling and exposes
  `nativeHandle()` for the existing C API. A high-level Arduino `Client`/`Server`
  stream interface is outside this first version.

## Checks

```sh
export HYPERDHT_DIR=/home/jacke/agents/alberto/work/hyperdht-demo
export HOLESAIL_DIR=/home/jacke/agents/alberto/work/holesail-esp32
nix-shell tests/shell.nix --run tests/run.sh
```

Host ASan/UBSan checks cover wrapper lifecycle/failure/restart, independent
publication state, original Holesail URL vector, metadata sequence/retry,
HTTP framing/bounds, response backpressure, slot limits, idle close and IP
refresh. Mock transport tests do not establish actual P2P interoperability.
See BUILD-RESULTS.md for the cross-build and hardware evidence boundaries.

On the N16R8, a matched-firmware hardware check confirmed `loop()` execution
and `ready=1`; the operator then confirmed the webpage loads. One idle snapshot
used 29,816 bytes of the 8 MiB PSRAM (about 29.1 KiB). This is not a peak-memory
measurement or a performance comparison with Ghost Node.

## License

AGPL-3.0-or-later; see LICENSE. The wrapper adapts Ghost Node's public server
and HTTP parser. HyperDHT, Holesail and all third-party dependencies retain
their original licenses/notices; those source trees are not copied into this
repository. Nothing is published or registered with Library Manager.
