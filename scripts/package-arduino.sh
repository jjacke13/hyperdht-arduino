#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
: "${IDF_PATH:?Enter the HyperDHT esp32 Nix shell}"
: "${HYPERDHT_DIR:?Set the private pinned HyperDHT checkout}"
: "${HOLESAIL_DIR:?Set the pinned Holesail codec checkout}"
: "${ARDUINO_SDK:?Set the installed Arduino-ESP32 3.3.6 esp32s3-libs directory}"
SODIUM_DIR=${SODIUM_DIR:-$PWD/examples/PublicWebServer/managed_components/espressif__libsodium}
for dir in "$HYPERDHT_DIR" "$HOLESAIL_DIR"; do
    [[ "$(git -C "$dir" rev-parse --show-toplevel)" == "$(realpath "$dir")" ]]
done
[[ "$(git -C "$HYPERDHT_DIR" rev-parse HEAD)" == bb16c0b0fab5c4dccfde50120955ea0a1730b903 ]]
[[ "$(git -C "$HOLESAIL_DIR" rev-parse HEAD)" == f57d423b2d65a660468cbb8029536ec99b9e79c2 ]]
[[ "$(git -C "$HYPERDHT_DIR/deps/libudx" rev-parse --show-toplevel)" == "$(realpath "$HYPERDHT_DIR/deps/libudx")" ]]
[[ "$(git -C "$HYPERDHT_DIR/deps/libudx" rev-parse HEAD)" == 759bf7641347f65a1896d4bc4c3b1606a459f466 ]]
git -C "$HYPERDHT_DIR" apply --reverse --check "$PWD/embedded-tls.patch"
[[ $(basename "$ARDUINO_SDK") == 3.3.6 && $(basename "$(dirname "$ARDUINO_SDK")") == esp32s3-libs ]]
grep -q 'SODIUM_VERSION_STRING "1.0.21"' "$SODIUM_DIR/port_include/sodium/version.h"

cmake -S arduino -B build-arduino-native -G Ninja \
    -DCMAKE_C_COMPILER=xtensa-esp32s3-elf-gcc -DCMAKE_CXX_COMPILER=xtensa-esp32s3-elf-g++ \
    -DARDUINO_SDK="$ARDUINO_SDK" -DHYPERDHT_DIR="$HYPERDHT_DIR" \
    -DHOLESAIL_DIR="$HOLESAIL_DIR" -DSODIUM_DIR="$SODIUM_DIR"
cmake --build build-arduino-native --target hyperdht_native -j2

# Stage only public API, a matched archive and the unconfigured example.
stage=$(mktemp -d "$PWD/dist-stage.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
package="$stage/HyperDHT_ESP32"
mkdir -p "$package/src/esp32s3" "$package/src/hyperdht" \
    "$package/examples/PublicWebServer" "$package/extras/sources"
cp arduino/library.properties LICENSE "$package/"
cp src/HyperDHTNode.h src/HolesailWebServer.h "$package/src/"
cp "$HYPERDHT_DIR/include/hyperdht/hyperdht.h" "$package/src/hyperdht/"
cp build-arduino-native/libhyperdht_native.a "$package/src/esp32s3/"
xtensa-esp32s3-elf-objcopy --strip-debug "$package/src/esp32s3/libhyperdht_native.a"
cp examples/PublicWebServer/PublicWebServer.ino "$package/examples/PublicWebServer/"
cat > "$package/src/HyperDHTTarget.h" <<'EOF'
#pragma once
#include <sdkconfig.h>
#include <esp_arduino_version.h>
#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ESP_ARDUINO_VERSION != ESP_ARDUINO_VERSION_VAL(3, 3, 6)
#error "This native package requires ESP32-S3 and Arduino-ESP32 3.3.6"
#endif
#if CONFIG_FREERTOS_UNICORE
#error "This native package uses Arduino's stock dual-core SDK"
#endif
EOF
sed -i '/#pragma once/a #include "HyperDHTTarget.h"' "$package/src/HyperDHTNode.h"

# Include corresponding native source, not just an opaque binary distribution.
git -C "$HYPERDHT_DIR" archive --format=tar.gz HEAD > "$package/extras/sources/hyperdht-cpp.tar.gz"
git -C "$HYPERDHT_DIR/deps/libudx" archive --format=tar.gz HEAD > "$package/extras/sources/libudx.tar.gz"
git -C "$HOLESAIL_DIR" archive --format=tar.gz HEAD > "$package/extras/sources/holesail-cpp.tar.gz"
tar -czf "$package/extras/sources/libsodium.tar.gz" --exclude=.git -C "$SODIUM_DIR" \
    CMakeLists.txt Kconfig LICENSE idf_component.yml libsodium port port_include
cp embedded-tls.patch "$package/extras/sources/"
tar -czf "$package/extras/sources/arduino-wrapper.tar.gz" \
    arduino src scripts/package-arduino.sh scripts/check-tls.py embedded-tls.patch LICENSE
printf '%s\n' 'HyperDHT bb16c0b + embedded-tls.patch; UDX 759bf764; Holesail f57d423; libsodium 1.0.21.' \
    'Native archive rebuilt against Arduino-ESP32 3.3.6 stock ESP32-S3 SDK, not ESP-IDF component archives.' \
    'Evaluation only: compile/link checks do not establish hardware startup or client interoperability.' \
    'Extract source archives and place libudx under hyperdht-cpp/deps/libudx; apply embedded-tls.patch.' \
    'See scripts/package-arduino.sh and arduino/CMakeLists.txt in arduino-wrapper.tar.gz to reproduce.' \
    > "$package/extras/SOURCES.txt"
sha256sum "$ARDUINO_SDK"/flags/* "$ARDUINO_SDK/qio_qspi/include/sdkconfig.h" \
    | sed "s|$ARDUINO_SDK/||g" > "$package/extras/sdk-sha256.txt"
mkdir -p dist
python - "$stage" "$PWD/dist/HyperDHT_ESP32-0.1.0.zip" <<'PY'
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import sys
root, output = Path(sys.argv[1]), Path(sys.argv[2])
with ZipFile(output, 'w', ZIP_DEFLATED) as archive:
    for path in sorted(root.rglob('*')):
        if path.is_file():
            archive.write(path, path.relative_to(root))
print(output)
PY
