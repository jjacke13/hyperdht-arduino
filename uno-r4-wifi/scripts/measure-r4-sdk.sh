#!/usr/bin/env bash
# Compile/link only. Use an Arduino CLI with renesas_uno 1.6.0 installed.
set -euo pipefail
cd "$(dirname "$0")/.."
toolchain=$(realpath "${1:?Arm GCC 7-2017q4 toolchain directory required}")
sodium=$(realpath "${2:?hard-float libsodium build directory required}")
mkdir -p build-r4-sdk
out=$(mktemp -d "$PWD/build-r4-sdk/link.XXXXXXXX")
echo "Evidence: $out"
printf 'INCOMPLETE\n' > "$out/report.txt"
tools=$toolchain/bin/arm-none-eabi
headers=$sodium/src/libsodium/include
archive=$sodium/src/libsodium/.libs/libsodium.a
test -f "$archive"
"$tools-gcc" --version > "$out/compiler.txt"
arduino-cli version > "$out/arduino-cli.txt"
flags=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
       -Os -ffunction-sections -fdata-sections -fstack-usage)
objects=()
for unit in rpc discovery udx_rx record noise transport handshake connector; do
    "$tools-gcc" "${flags[@]}" -Iinclude -I"$headers" -std=c99 -c "src/$unit.c" -o "$out/$unit.o"
    objects+=("$out/$unit.o")
done
"$tools-ar" rcs "$out/libhdtiny.a" "${objects[@]}"
"$tools-gcc" "${flags[@]}" -Iinclude -I"$headers" -std=c99 -Dmain=hdt_r4_measure_main \
    -c platform/ra4m1/measure.c -o "$out/measure.o"
"$tools-ar" rcs "$out/libhdtiny.a" "$out/measure.o"
"$tools-nm" -g --defined-only "${objects[@]}" | awk '$2 == "T" && $3 ~ /^hdt_/ {print $3}' > "$out/required-apis.txt"
roots=$(awk '{printf " -Wl,-u,%s", $1}' "$out/required-apis.txt")
fqbn=arduino:renesas_uno:unor4wifi
compiler_property="runtime.tools.arm-none-eabi-gcc-7-2017q4.path=$toolchain"

for profile in fallback sce5 sce5-wifi sce5-wifi-stack8k; do
    sketch=$out/$profile/LinkProbe
    build=$out/$profile/build
    profile_roots=$roots
    variant_property=()
    mkdir -p "$sketch/src" "$build"
    cat > "$sketch/LinkProbe.ino" <<'EOF'
extern "C" int hdt_r4_measure_main(void);
void setup() { (void)hdt_r4_measure_main(); }
void loop() {}
EOF
    cp platform/ra4m1/entropy.c "$sketch/src/entropy_callback.c"
    if [[ $profile != fallback ]]; then
        cp platform/arduino-r4/entropy.{c,h} "$sketch/src/"
    fi
    if [[ $profile == sce5-wifi* ]]; then
        cp platform/arduino-r4/wifi_link_probe.cpp "$sketch/src/"
        profile_roots+=' -Wl,-u,hdt_r4_wifi_link_probe'
    fi
    if [[ $profile == sce5-wifi-stack8k ]]; then
        arduino-cli compile --show-properties=expanded --fqbn "$fqbn" \
            --build-property "$compiler_property" "$sketch" > "$out/$profile/sdk-properties.txt"
        variant=$(sed -n 's/^build.variant.path=//p' "$out/$profile/sdk-properties.txt")
        overlay="$out/$profile/UNOWIFIR4"
        python3 scripts/r4-stack-overlay.py "$variant" "$overlay"
        variant_property=(--build-property "build.variant.path=$overlay")
    fi
    arduino-cli compile --verbose --fqbn "$fqbn" --build-path "$build" \
        --build-property "$compiler_property" "${variant_property[@]}" \
        --build-property "compiler.c.extra_flags=\"-I$headers\" -fstack-usage" \
        --build-property "compiler.cpp.extra_flags=\"-I$headers\"" \
        --build-property "compiler.c.elf.extra_flags=$profile_roots" \
        --build-property "compiler.libraries.ldflags=\"$out/libhdtiny.a\" \"$archive\"" \
        "$sketch" > "$out/$profile/compile.log" 2>&1
    grep -E 'Using board .*renesas_uno/1\.6\.0' "$out/$profile/compile.log" > /dev/null
    elf=$build/LinkProbe.ino.elf
    "$tools-nm" --defined-only "$elf" > "$out/$profile/symbols.txt"
    "$tools-nm" -C --defined-only "$elf" > "$out/$profile/symbols-demangled.txt"
    "$tools-nm" -u "$elf" > "$out/$profile/undefined.txt"
    test ! -s "$out/$profile/undefined.txt"
    "$tools-size" -A "$elf" > "$out/$profile/sections.txt"
    "$tools-objdump" -h "$elf" > "$out/$profile/section-addresses.txt"
    if [[ $profile == sce5-wifi-stack8k ]]; then
        "$tools-objcopy" -O binary -j .text "$elf" "$out/$profile/text.bin"
    fi
    while IFS= read -r api; do
        grep -q " T $api\$" "$out/$profile/symbols.txt"
    done < "$out/required-apis.txt"
    sha256sum "$elf" "$build/LinkProbe.ino.map" > "$out/$profile/artifacts.sha256"
done
grep -q ' W hdt_ra4m1_entropy$' "$out/fallback/symbols.txt"
for symbol in hdt_ra4m1_entropy HW_SCE_McuSpecificInit HW_SCE_RNG_Read; do
    grep -q " T $symbol\$" "$out/sce5/symbols.txt"
    grep -q " T $symbol\$" "$out/sce5-wifi/symbols.txt"
    grep -q " T $symbol\$" "$out/sce5-wifi-stack8k/symbols.txt"
    if grep -q " T $symbol\$" "$out/fallback/symbols.txt"; then
        echo "Unexpected SCE symbol in fallback image: $symbol" >&2
        exit 1
    fi
done
grep -E 'Using library WiFiS3 .*renesas_uno/1\.6\.0/libraries/WiFiS3' "$out/sce5-wifi/compile.log" > /dev/null
grep -E 'Using library WiFiS3 .*renesas_uno/1\.6\.0/libraries/WiFiS3' "$out/sce5-wifi-stack8k/compile.log" > /dev/null
for symbol in 'CWifi::begin(' 'CWifi::status(' 'CWifi::disconnect(' 'WiFiUDP::begin(' \
              'WiFiUDP::beginPacket(' 'WiFiUDP::write(' 'WiFiUDP::endPacket(' \
              'WiFiUDP::parsePacket(' 'WiFiUDP::read(' 'WiFiUDP::remoteIP(' \
              'WiFiUDP::remotePort(' 'WiFiUDP::stop('; do
    grep -Fq "$symbol" "$out/sce5-wifi/symbols-demangled.txt"
    grep -Fq "$symbol" "$out/sce5-wifi-stack8k/symbols-demangled.txt"
    if grep -Fq "$symbol" "$out/sce5/symbols-demangled.txt"; then
        echo "Unexpected networking symbol in SCE5-only image: $symbol" >&2
        exit 1
    fi
done

# Use loadable section addresses for flash, and linker heap/stack boundaries for
# RAM. Arduino's printed global count omits reservations and alignment.
python3 - "$out" <<'PY' > "$out/report.txt.tmp"
import pathlib
import re
import sys

out = pathlib.Path(sys.argv[1])
figures = {}
for profile in ("fallback", "sce5", "sce5-wifi", "sce5-wifi-stack8k"):
    folder = out / profile
    symbols = {
        parts[2]: int(parts[0], 16)
        for line in (folder / "symbols.txt").read_text().splitlines()
        if len(parts := line.split()) == 3
    }
    flash_end = 0x4000
    for line in re.findall(r"^\s*\d+\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+).*\n[^\n]*\bLOAD\b",
                           (folder / "section-addresses.txt").read_text(), re.MULTILINE):
        size, address = map(lambda value: int(value, 16), line)
        if 0x4000 <= address < 0x40000:
            flash_end = max(flash_end, address + size)
    flash = flash_end - 0x4000
    static = symbols["__HeapBase"] - 0x20000000
    stack = symbols["__StackTop"] - symbols["__StackLimit"]
    vector = 0x20008000 - symbols["__StackTop"]
    spare = 32768 - static - 8192 - vector
    if not (0 < flash <= 245760 and 0 < static < 32768 and stack > 0 and vector >= 0):
        sys.exit(f"{profile} has an unexpected map")
    if profile == "sce5-wifi-stack8k":
        sections = {
            row[1]: (int(row[2], 16), int(row[3], 16))
            for line in (folder / "section-addresses.txt").read_text().splitlines()
            if len(row := line.split()) >= 7 and row[0].isdigit()
        }
        top, bottom = symbols["__StackTop"], symbols["__StackLimit"]
        base, limit = symbols["__HeapBase"], symbols["__HeapLimit"]
        vectors = (folder / "text.bin").read_bytes()[:8]
        reset_sp = int.from_bytes(vectors[:4], "little")
        reset_pc = int.from_bytes(vectors[4:], "little")
        if not (stack == 8192 and vector == 256 and reset_sp == top == symbols["__StackTopAll"]
                and sections[".text"][1] == 0x4000 and reset_pc == symbols["Reset_Handler"] | 1
                and base < limit == bottom and "g_heap" not in symbols
                and top - 1024 == symbols["g_main_stack"]
                and sections[".stack_dummy"] == (8192, bottom)
                and sections[".heap"] == (limit - base, base)
                and sections[".vector_table"] == (256, top)):
            sys.exit("8 KiB stack, reset vector or heap range verification failed")
        print(f"Verified reset SP {reset_sp:#010x}, stack {bottom:#010x}–{top:#010x}, "
              f"heap {base:#010x}–{limit:#010x} ({limit - base} B)")
    figures[profile] = flash, static, spare
    print(f"{profile}: flash span {flash}/245760 B, static RAM below heap {static} B, "
          f"SDK main stack {stack} B, RAM vectors {vector} B, "
          f"unassigned RAM with an 8192 B stack budget {spare} B")
flash, static, spare = figures["sce5-wifi"]
print(f"WiFiS3 increment: {flash - figures['sce5'][0]} B flash, "
      f"{static - figures['sce5'][1]} B static RAM")
print("PASS: all images linked, including the WiFiS3 probe with an 8 KiB stack.")
if spare < 0:
    print("The WiFiS3 image exceeds the 8192 B stack budget before dynamic allocation.")
else:
    print("The stack8k profile reserves the full stack and bounds the heap. Peak use is unmeasured.")
PY
mv "$out/report.txt.tmp" "$out/report.txt"
cat "$out/report.txt"
