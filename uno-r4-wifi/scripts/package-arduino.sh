#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/.."
toolchain=$(realpath "${1:?Arm GCC 7-2017q4 toolchain directory required}")
sodium=$(realpath "${2:?Hard-float libsodium build directory required}")
sodium_source=$(realpath "${3:?Unbuilt libsodium 1.0.20 source directory required}")
headers=$sodium/src/libsodium/include
archive=$sodium/src/libsodium/.libs/libsodium.a
test -f "$archive"
grep -Fq '#define SODIUM_VERSION_STRING "1.0.20"' "$headers/sodium/version.h"
grep -Fqx "PACKAGE_VERSION='1.0.20'" "$sodium_source/configure"
test ! -e "$sodium_source/config.status"

mkdir -p build-r4-sdk
out=$(mktemp -d "$PWD/build-r4-sdk/package.XXXXXXXX")
echo "Package: $out" >&2
stage=$out/stage
package=$stage/HyperDHT_Tiny_R4
native=$package/src/cortex-m4
extras=$package/extras
mkdir -p "$native" "$package/src/hdtiny" "$package/examples" "$extras/sources"
cp -R src include "$out/"
patch --batch --forward --fuzz=0 -d "$out" -p1 < examples/uno-r4-echo/patches/connector.patch >&2

tools=$toolchain/bin/arm-none-eabi
flags=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
       -Os -ffunction-sections -fdata-sections -std=c99
       -I"$out/include" -I"$headers")
objects=()
for unit in rpc discovery udx_rx record noise transport handshake connector punch listener publish publish_walk listener_punch; do
    "$tools-gcc" "${flags[@]}" -c "$out/src/$unit.c" -o "$out/$unit.o"
    objects+=("$out/$unit.o")
    if [[ -f "$out/include/hdtiny/$unit.h" ]]; then
        cp "$out/include/hdtiny/$unit.h" "$package/src/hdtiny/"
    fi
done
"$tools-ar" rcsD "$native/libhdtiny_r4.a" "${objects[@]}"
cp "$archive" "$native/libsodium.a"
"$tools-strip" -D -g "$native/"*.a

cp arduino/library.properties LICENSE PROVENANCE.md "$package/"
cp -R licenses "$package/"
cp "$sodium_source/LICENSE" "$package/licenses/libsodium.txt"
cp -R "$headers/"sodium{.h,} "$package/src/"
cp arduino/HyperDHT{Client,Server}.{h,cpp} platform/arduino-r4/entropy.{c,h} "$package/src/"
cp platform/ra4m1/entropy.c "$package/src/entropy_callback.c"
# Reject an echo template edited with local network credentials.
for name in HyperDHTEcho HyperDHTListenEcho; do
    sketch=examples/$name/$name.ino
    grep -Fqx 'static const char ssid[] = "YOUR_WIFI_SSID";' "$sketch"
    grep -Fqx 'static const char password[] = "YOUR_WIFI_PASSWORD";' "$sketch"
    mkdir "$package/examples/$name"
    cp "$sketch" "$package/examples/$name/"
done
cp ARDUINO-CLI.md "$package/"
cp scripts/r4-stack-overlay.py arduino/compile-r4.sh \
   examples/uno-r4-echo/{hyperdht-echo.cjs,patches/wifis3-1.6.0.patch} \
   examples/uno-r4-listener/hyperdht-client.cjs "$extras/"
tar -czf "$extras/sources/hyperdht-tiny-r4.tar.gz" \
    arduino src include licenses LICENSE PROVENANCE.md ARDUINO-CLI.md \
    docs/R4-SDK-LINK.md scripts/{package-arduino.sh,r4-stack-overlay.py} \
    platform/{arduino-r4,ra4m1}/entropy.* \
    examples/uno-r4-echo/{patches,hyperdht-echo.cjs} examples/uno-r4-listener/hyperdht-client.cjs \
    examples/{HyperDHTEcho/HyperDHTEcho,HyperDHTListenEcho/HyperDHTListenEcho}.ino
tar -czf "$extras/sources/libsodium.tar.gz" -C "$sodium_source" .
version=$(sed -n 's/^version=//p' arduino/library.properties)
"${PYTHON:-python3}" - "$stage" "$out/HyperDHT_Tiny_R4-$version.zip" <<'PY'
import sys
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

stage, output = map(Path, sys.argv[1:])
with ZipFile(output, "w", ZIP_DEFLATED) as archive:
    for path in sorted(stage.rglob("*")):
        if path.is_file():
            archive.write(path, path.relative_to(stage))
print(output)
PY
