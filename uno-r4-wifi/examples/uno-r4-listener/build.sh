#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/../.."
toolchain=$(realpath "${1:?Arm GCC 7-2017q4 toolchain directory required}")
sodium=$(realpath "${2:?Hard-float libsodium build directory required}")
cli=${ARDUINO_CLI:-arduino-cli}
archive=$sodium/src/libsodium/.libs/libsodium.a
headers=$sodium/src/libsodium/include
test -f "$archive"

mkdir -p build-r4-sdk
out=$(mktemp -d "$PWD/build-r4-sdk/listener.XXXXXXXX")
echo "Build: $out"
sketch=$out/HdtListen
mkdir -p "$sketch/src/hdtiny"
cp examples/uno-r4-listener/HdtListen/{HdtListen.ino,echo_session.h,listener_settings.h} "$sketch/"
cp src/{listener,listener_punch,publish,publish_walk,connector,handshake,noise,record,rpc,transport,udx_rx}.c src/compact_internal.h src/listener_punch.h "$sketch/src/"
cp include/hdtiny/*.h "$sketch/src/hdtiny/"
cp platform/arduino-r4/entropy.{c,h} "$sketch/src/"
cp platform/ra4m1/entropy.c "$sketch/src/entropy_callback.c"

fqbn=arduino:renesas_uno:unor4wifi
compiler="runtime.tools.arm-none-eabi-gcc-7-2017q4.path=$toolchain"
"$cli" compile --show-properties=expanded --fqbn "$fqbn" \
    --build-property "$compiler" "$sketch" > "$out/sdk-properties.txt"
variant=$(sed -n 's/^build.variant.path=//p' "$out/sdk-properties.txt")
"${PYTHON:-python3}" scripts/r4-stack-overlay.py "$variant" "$out/UNOWIFIR4"
wifi=$out/WiFiS3
cp -R "$variant/../../libraries/WiFiS3" "$wifi"
chmod -R u+w "$wifi"
patch --batch --forward --fuzz=0 -d "$wifi" -p1 < examples/uno-r4-echo/patches/wifis3-1.6.0.patch
if ! "$cli" compile --fqbn "$fqbn" --verbose --library "$wifi" \
    --build-path "$out/build" --build-property "$compiler" \
    --build-property "build.variant.path=$out/UNOWIFIR4" \
    --build-property "compiler.c.extra_flags=\"-I$headers\" \"-I$sketch/src\" -fstack-usage" \
    --build-property "compiler.cpp.extra_flags=\"-I$headers\" \"-I$sketch/src\" -fstack-usage" \
    --build-property "compiler.libraries.ldflags=\"$archive\"" \
    "$sketch" > "$out/compile.log" 2>&1; then
    tail -35 "$out/compile.log" >&2
    exit 1
fi

grep -Fq "in folder: $wifi" "$out/compile.log"
elf=$out/build/HdtListen.ino.elf
tools=$toolchain/bin/arm-none-eabi
"$tools-nm" -n "$elf" > "$out/symbols.txt"
test -z "$("$tools-nm" -u "$elf")"
for symbol in hdt_listener_handshake hdt_listener_punch_reply hdt_publish_walk_poll hdt_publish_poll HW_SCE_RNG_Read; do
    grep -q " T $symbol$" "$out/symbols.txt"
done
if grep -Eq ' T hdt_(connector_begin|find_begin)$' "$out/symbols.txt"; then
    echo "Unexpected client entry point in the listener image" >&2
    exit 1
fi
"$tools-size" "$elf"
sha256sum "$out/build/HdtListen.ino.bin"
