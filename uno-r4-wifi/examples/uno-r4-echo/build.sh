#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
set -euo pipefail
cd "$(dirname "$0")/../.."
example=$PWD/examples/uno-r4-echo
toolchain=$(realpath "${1:?Arm GCC 7-2017q4 toolchain directory required}")
sodium=$(realpath "${2:?hard-float libsodium build directory required}")
cli=${ARDUINO_CLI:-arduino-cli}
tools=$toolchain/bin/arm-none-eabi
headers=$sodium/src/libsodium/include
archive=$sodium/src/libsodium/.libs/libsodium.a
test -f "$archive"
mkdir -p build-r4-sdk
out=$(mktemp -d "$PWD/build-r4-sdk/echo.XXXXXXXX")
echo "Build: $out"

cp -R src include "$out/"
patch --batch --forward --fuzz=0 -d "$out" -p1 < "$example/patches/connector.patch"
sketch=$out/HdtEcho
mkdir -p "$sketch/src"
cp "$example/HdtEcho/HdtEcho.ino" "$sketch/"
cp platform/arduino-r4/entropy.{c,h} "$sketch/src/"
cp platform/ra4m1/entropy.c "$sketch/src/entropy_callback.c"

fqbn=arduino:renesas_uno:unor4wifi
compiler_property="runtime.tools.arm-none-eabi-gcc-7-2017q4.path=$toolchain"
"$cli" compile --show-properties=expanded --fqbn "$fqbn" \
    --build-property "$compiler_property" "$sketch" > "$out/sdk-properties.txt"
variant=$(sed -n 's/^build.variant.path=//p' "$out/sdk-properties.txt")
if [[ $variant != */renesas_uno/1.6.0/variants/UNOWIFIR4 ]]; then
    echo "Expected Arduino Renesas UNO 1.6.0, got: $variant" >&2
    exit 1
fi
"${PYTHON:-python3}" scripts/r4-stack-overlay.py "$variant" "$out/UNOWIFIR4"
wifi=$out/WiFiS3
cp -R "$variant/../../libraries/WiFiS3" "$wifi"
chmod -R u+w "$wifi"
patch --batch --forward --fuzz=0 -d "$wifi" -p1 < "$example/patches/wifis3-1.6.0.patch"

flags=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
       -Os -ffunction-sections -fdata-sections -std=c99
       -I"$out/include" -I"$out/src" -I"$headers")
objects=()
for unit in rpc discovery udx_rx record noise transport handshake connector punch; do
    "$tools-gcc" "${flags[@]}" -c "$out/src/$unit.c" -o "$out/$unit.o"
    objects+=("$out/$unit.o")
done
"$tools-gcc" "${flags[@]}" -Dmain=hdt_r4_measure_main -c "$example/session.c" -o "$out/measure.o"
"$tools-ar" rcs "$out/libhdtiny.a" "${objects[@]:0:8}" "$out/measure.o" "$out/punch.o"
roots=$("$tools-nm" -g --defined-only "${objects[@]}" |
    awk '$2 == "T" && $3 ~ /^hdt_/ {printf " -Wl,-u,%s", $3}')

if ! "$cli" compile --library "$wifi" --warnings all --verbose --fqbn "$fqbn" \
    --build-path "$out/build" --build-property "$compiler_property" \
    --build-property "build.variant.path=$out/UNOWIFIR4" \
    --build-property "compiler.c.extra_flags=\"-I$headers\"" \
    --build-property "compiler.cpp.extra_flags=\"-I$headers\" \"-I$out/include\" \"-I$example\" -fstack-usage" \
    --build-property "compiler.c.elf.extra_flags=$roots -Wl,--wrap=_sbrk -Wl,--wrap=abort" \
    --build-property "compiler.libraries.ldflags=\"$out/libhdtiny.a\" \"$archive\"" \
    "$sketch" > "$out/compile.log" 2>&1; then
    tail -30 "$out/compile.log" >&2
    exit 1
fi
grep -Fq "in folder: $wifi" "$out/compile.log"
elf=$out/build/HdtEcho.ino.elf
test -z "$("$tools-nm" -u "$elf")"
"$tools-size" "$elf"
sha256sum "$out/build/HdtEcho.ino.bin"
