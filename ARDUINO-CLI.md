# ESP32-S3 library for the Arduino framework and IDE

**ESP32-S3 hardware only.** Arduino is the software framework/IDE here;
this package does not support Uno, Mega or Uno R4 boards.

Tested hardware variants:

| ESP32-S3 variant | Flash | PSRAM |
|---|---:|---|
| **N4R2** | 4 MB | 2 MB quad (QSPI) |
| **N16R8** | 16 MB | 8 MB octal (OPI) |

These are the reported hardware tests, not a compatibility claim for all
ESP32 boards. The packaged native library requires Arduino-ESP32 **3.3.6**.

The generated `dist/HyperDHT_ESP32-0.1.0.zip` installs as an ordinary Arduino
library. It exposes `HyperDHTNode` and `HolesailWebServer` and includes the
small `PublicWebServer` sketch. No display or board-specific GPIO dependency.
The existing ESP-IDF component build remains supported.

This first package targets **ESP32-S3, stock Arduino-ESP32 3.3.6 only**.
It contains a native archive rebuilt against that core's actual SDK headers,
configuration and compiler flags, rather than reusing the ESP-IDF component
archives. A normal Arduino library cannot express the dependencies' CMake
source selection, UDX hardening and per-library compile definitions. The
archive carries those settings without requiring users to patch the core.
Public headers reject a different chip/core version or a unicore SDK.

## Build the package with Nix

Start from the existing README's pinned private dependency checkouts and
ESP-IDF component dependency setup, which supplies libsodium 1.0.21. Install
Arduino-ESP32 3.3.6 using Arduino CLI's Espressif package index. Prefer an
isolated CLI config/sketchbook when evaluating; do not overwrite existing
Arduino installations.

```sh
export HYPERDHT_DIR=/path/to/private/hyperdht-cpp
export HOLESAIL_DIR=/path/to/private/holesail-cpp
export ARDUINO_SDK=/path/to/arduino-data/packages/esp32/tools/esp32s3-libs/3.3.6
nix develop "$HYPERDHT_DIR#esp32"
cd /path/to/hyperdht-arduino
bash scripts/package-arduino.sh
```

The archive keeps the embedded RNG/TLS correction, UDX reset/teardown guards,
no-exceptions/no-RTTI native profile and libsodium 1.0.21. The latter is bundled
instead of taking the stock SDK's older libsodium. Native code is compiled
against the stock dual-core SDK; this is a distinct image from the previously
tested unicore component firmware.

The ZIP contains corresponding native source archives, the embedded patch,
licenses, rebuild CMake/script, pinned revisions and SDK flag/config hashes.
Generated ZIPs and build directories stay ignored. Only the unconfigured
example enters the ZIP; local Wi-Fi files, SDK configs and identities do not.

## Install and compile

Use Arduino IDE's **Add .ZIP Library**, or Arduino CLI's `lib install
--zip-path /path/to/HyperDHT_ESP32-0.1.0.zip`. CLI ZIP installation requires
`library.enable_unsafe_install=true`; enable that only in the intended
evaluation config. With a custom config, add `--config-file /path/to/config.yaml`
to each CLI command below.

On NixOS, use `nix shell nixpkgs#arduino-cli nixpkgs#python3`; the packaged CLI
provides its compiler runtime environment, while Python is needed by the ESP32
build recipes. This evaluation used CLI 1.4.1 and core 3.3.6; Axiometa's source
pins CLI 1.5.1, so that exact CLI version remains an integration check.

For Arduino IDE on NixOS, launch the IDE from an environment providing
`python3` in PATH. This was needed for the operator's successful IDE workflow.

Compile the installed example (replace `SKETCH` with its actual directory):

```sh
# N4R2: 4 MB flash, quad PSRAM, 3 MB application partition, native USB serial.
arduino-cli compile --jobs 2 --build-path build-arduino-cli/n4r2 \
  --fqbn 'esp32:esp32:esp32s3:FlashSize=4M,FlashMode=dio,PSRAM=enabled,PartitionScheme=huge_app,USBMode=hwcdc,CDCOnBoot=cdc' \
  SKETCH

# N16R8: 16 MB flash and octal PSRAM; use a separate build directory.
arduino-cli compile --jobs 2 --build-path build-arduino-cli/n16r8 \
  --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,FlashMode=dio,PSRAM=opi,PartitionScheme=huge_app,USBMode=hwcdc,CDCOnBoot=cdc' \
  SKETCH
```

These generic Espressif FQBNs verify packaging; they do not replace Axiometa's
unavailable authenticated Genesis Mini board definitions. Select the actual
board's memory and USB profile before any hardware use.

The example keeps `SET_LOOP_TASK_STACK_SIZE(32768)` at file scope. Keep that
setting when writing a server sketch, and call the APIs only from the owning
`setup()`/`loop()` task. It polls asynchronously; do not call methods from Wi-Fi
callbacks or interrupt handlers. Public mode accepts anyone knowing the link.

The example ships with a placeholder Wi-Fi name. For a local CLI build, place
an ignored `wifi_config.h` beside the `.ino`:

```cpp
#define P2P_WIFI_SSID "YOUR_WIFI_SSID"
#define P2P_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
```

Use actual credentials only in that private local file. The ESP-IDF build
continues using its existing menuconfig settings instead. Identity remains in
the example's `p2p-arduino` NVS namespace across ordinary restarts; this package
does not contain a seed or change the original Ghost Node application.

## Verified scope — 2026-10-26

The ZIP installed successfully into an isolated Arduino CLI sketchbook. Both
generic ESP32-S3 profiles compiled through final ELF/image creation using only
the installed library and core. Required full HyperDHT server, mutable
publication and encrypted stream symbols remain linked; GCC TLS is zero in
both ELFs. Disassembly confirms the example's loop stack function returns
32,768 bytes. A non-S3 target was rejected during preprocessing.

| Profile | CLI-reported program size | Static globals | App partition |
|---|---:|---:|---:|
| N4R2 generic | 1,246,019 B | 44,920 B | 3,145,728 B |
| N16R8 generic | 1,248,279 B | 45,304 B | 3,145,728 B |

These are build figures, not runtime heap/PSRAM measurements. The compiler's
static-memory report excludes dynamic allocations and task stacks. This is
not a controlled size comparison against the custom ESP-IDF profile.

Existing host ASan/UBSan wrapper/HTTP checks pass. Both ESP-IDF profiles also
rebuild and retain zero GCC TLS and required server symbols after the portable
Wi-Fi configuration and loop-stack declaration changes.

The operator subsequently confirmed the Arduino IDE build/upload workflow
after adding Python3 to PATH through a temporary Nix shell, and reports tested
N4R2 and N16R8 hardware. These operator reports supplement the automated build
checks; they do not establish load/reconnect reliability or runtime memory
peaks. No agent flashing was performed. Exact Axiometa FQBN/CLI 1.5.1, Studio
ZIP acceptance and catalog/Library Manager integration remain separate checks.
Nothing has been published, registered or submitted to Axiometa.
