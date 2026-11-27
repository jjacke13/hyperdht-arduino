#!/usr/bin/env bash
# Run inside the existing pinned nix develop shell. No hardware access.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
out=$root/build-r4
mkdir -p "$out"
printf '%s\n' 'INCOMPLETE: measurement checks have not finished.' > "$out/report.txt"
sodium_source=$(nix eval --raw --impure --expr \
  '(import (builtins.getFlake (toString ./.)).inputs.nixpkgs { system = builtins.currentSystem; }).libsodium.src.outPath')
if [[ ! -d $sodium_source ]]; then
  nix build --no-link --impure --expr \
    '(import (builtins.getFlake (toString ./.)).inputs.nixpkgs { system = builtins.currentSystem; }).libsodium.src'
fi
flags=(-Os -g -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
  -ffunction-sections -fdata-sections -fstack-usage)
sodium_flags="${flags[*]} -DRANDOMBYTES_CUSTOM_IMPLEMENTATION -DRANDOMBYTES_DEFAULT_IMPLEMENTATION=NULL"
# A stamp prevents reusing crypto built with different flags/source.
stamp=$(printf '%s\n' "$sodium_source" "$sodium_flags" full-api-v1 "$(arm-none-eabi-gcc --version | head -1)" | sha256sum)
if [[ ! -f $out/sodium-stamp ]] || [[ $(cat "$out/sodium-stamp") != "$stamp" ]]; then
  # Keep prior builds rather than deleting an operator's artifacts.
  if [[ -d $out/sodium-src ]]; then
    mv "$out/sodium-src" "$out/sodium-src.previous.$(date +%s%N)"
  fi
  cp -R "$sodium_source" "$out/sodium-src"
  chmod -R u+w "$out/sodium-src"
  (
    cd "$out/sodium-src"
    env CC=arm-none-eabi-gcc AR=arm-none-eabi-ar RANLIB=arm-none-eabi-ranlib \
      CFLAGS="$sodium_flags" \
      LDFLAGS='--specs=nosys.specs -mcpu=cortex-m4 -mthumb -mfloat-abi=soft' \
      ./configure --host=arm-none-eabi --disable-shared --enable-static \
      --disable-asm --disable-ssp --without-pthreads \
      > "$out/sodium-configure.log" 2>&1
    make -j4 > "$out/sodium-build.log" 2>&1
  )
  printf '%s\n' "$stamp" > "$out/sodium-stamp"
fi
includes=(-Iinclude -I"$out/sodium-src/src/libsodium/include")
objects=()
units=(rpc discovery udx_rx record noise transport handshake connector)
sources=()
headers=()
for unit in "${units[@]}"; do
  arm-none-eabi-gcc "${flags[@]}" -std=c99 -Wall -Wextra -Werror -pedantic \
    "${includes[@]}" -c "src/$unit.c" -o "$out/$unit.o"
  objects+=("$out/$unit.o")
  sources+=("src/$unit.c")
  headers+=("include/hdtiny/$unit.h")
done
printf '%s\n' 'DIRECT CONNECTOR LINK HARNESS: actual connector/discovery, target crypto; board I/O absent.' > "$out/profile.txt"
# Retain every exported tiny API, even if the smoke path does not call it.
mapfile -t api < <(arm-none-eabi-nm -g --defined-only "${objects[@]}" | awk '$2 == "T" && $3 ~ /^hdt_/ {print $3}')
roots=()
for symbol in "${api[@]}"; do roots+=("-Wl,--undefined=$symbol"); done
for unit in measure entropy startup; do
  arm-none-eabi-gcc "${flags[@]}" -std=c99 -Wall -Wextra -Werror \
    "${includes[@]}" -c "platform/ra4m1/$unit.c" -o "$out/$unit.o"
done
arm-none-eabi-gcc "${flags[@]}" -nostartfiles --specs=nosys.specs \
  -T platform/ra4m1/measure.ld -Wl,--gc-sections,--print-memory-usage \
  "-Wl,-Map=$out/measure.map" "${roots[@]}" "${objects[@]}" \
  "$out/measure.o" "$out/entropy.o" "$out/startup.o" \
  "$out/sodium-src/src/libsodium/.libs/libsodium.a" \
  -o "$out/measure.elf" > "$out/link.log" 2>&1
arm-none-eabi-nm -u "$out/measure.elf" > "$out/undefined.txt"
test ! -s "$out/undefined.txt"
arm-none-eabi-nm -g --defined-only "$out/measure.elf" > "$out/symbols.txt"
for symbol in "${api[@]}"; do grep -q " $symbol\$" "$out/symbols.txt"; done
if grep -Eq ' (malloc|calloc|realloc|sodium_malloc|_sbrk|_read|_write|_close|_lseek)$' "$out/symbols.txt"; then
  echo 'Unexpected allocator retained; inspect the map before reporting no heap.' >&2
  exit 1
fi
arm-none-eabi-readelf -h -l -A "$out/measure.elf" > "$out/elf.txt"
grep -q 'Machine:.*ARM' "$out/elf.txt"
! grep -q INTERP "$out/elf.txt"
arm-none-eabi-size -A "$out/measure.elf" > "$out/sections.txt"
arm-none-eabi-nm --defined-only "$out/measure.elf" > "$out/all-symbols.txt"
find "$out/sodium-src/src/libsodium" -name '*.su' -exec cat {} + > "$out/all-frames.txt"
cat "$out"/*.su >> "$out/all-frames.txt"
awk 'NR == FNR {symbols[$3]=1; next}
  {n=split($1,a,":"); if (a[n] in symbols) print}' \
  "$out/all-symbols.txt" "$out/all-frames.txt" | sort -k2,2nr > "$out/retained-frames.txt"
sha256sum "${sources[@]}" "${headers[@]}" src/compact_internal.h \
  platform/ra4m1/* scripts/measure-r4.sh flake.lock > "$out/inputs.sha256"
cat > "$out/layout.c" <<'EOF'
#include "hdtiny/discovery.h"
#include "hdtiny/connector.h"
/* Unlinked sizeof probes, not additional target storage. */
unsigned char hdt_size_connector[sizeof(hdt_connector)];
unsigned char hdt_size_discovery[sizeof(hdt_discovery)];
unsigned char hdt_size_transport[sizeof(hdt_transport)];
unsigned char hdt_size_record[sizeof(hdt_record)];
unsigned char hdt_size_noise[sizeof(hdt_noise)];
EOF
arm-none-eabi-gcc "${flags[@]}" -std=c99 "${includes[@]}" \
  -c "$out/layout.c" -o "$out/layout.o"
arm-none-eabi-nm --print-size --size-sort --radix=d "$out/layout.o" > "$out/layout.txt"
# Native functional check of the harness, using the actual host sodium RNG.
cc -std=c99 -O2 -Wall -Wextra -Werror -DHDT_MEASURE_HOST_TEST -Iinclude \
  platform/ra4m1/measure.c "${sources[@]}" \
  $(pkg-config --cflags --libs libsodium) -o "$out/host-self-check"
"$out/host-self-check"
# Negative link check: static RAM exceeding capacity must fail, not be omitted.
printf '%s\n' 'unsigned char hdt_overflow[32768];' | \
  arm-none-eabi-gcc "${flags[@]}" -x c -c - -o "$out/overflow.o"
if arm-none-eabi-gcc "${flags[@]}" -nostartfiles --specs=nosys.specs \
    -T platform/ra4m1/measure.ld -Wl,--undefined=hdt_overflow \
    "$out/overflow.o" "$out/startup.o" "$out/measure.o" "$out/entropy.o" \
    "${objects[@]}" "$out/sodium-src/src/libsodium/.libs/libsodium.a" \
    -Wl,--gc-sections -o "$out/overflow.elf" > "$out/overflow.log" 2>&1; then
  echo 'RAM overflow check unexpectedly linked' >&2; exit 1
fi
grep -q 'static RAM overlaps reserved 8 KiB stack' "$out/overflow.log"
printf '%s\n' 'const unsigned char hdt_flash_overflow[262144] = {1};' | \
  arm-none-eabi-gcc "${flags[@]}" -x c -c - -o "$out/flash-overflow.o"
if arm-none-eabi-gcc "${flags[@]}" -nostartfiles --specs=nosys.specs \
    -T platform/ra4m1/measure.ld -Wl,--undefined=hdt_flash_overflow \
    "$out/flash-overflow.o" "$out/startup.o" "$out/measure.o" "$out/entropy.o" \
    "${objects[@]}" "$out/sodium-src/src/libsodium/.libs/libsodium.a" \
    -Wl,--gc-sections -o "$out/flash-overflow.elf" > "$out/flash-overflow.log" 2>&1; then
  echo 'Flash overflow check unexpectedly linked' >&2; exit 1
fi
grep -q 'application flash overflow' "$out/flash-overflow.log"
{
  cat "$out/profile.txt"
  arm-none-eabi-gcc --version | head -1
  printf 'libsodium source: %s\n' "$sodium_source"
  printf 'source NAR hash: '; nix hash path "$sodium_source"
  printf 'Retained tiny APIs: %d\n' "${#api[@]}"
  # ld reports RAM 100% because the reserved stack ends at the top of SRAM.
  # Count static sections plus the explicit stack, excluding the free gap.
  awk '$1 == ".data" || $1 == ".bss" {s += $2} $1 == ".stack" {k = $2}
    END {printf "Static RAM: %d B; reserved stack: %d B; unassigned SRAM: %d B\n", s, k, 32768-s-k}' "$out/sections.txt"
  cat "$out/link.log" "$out/sections.txt" "$out/layout.txt"
  arm-none-eabi-nm --print-size --size-sort --radix=d "$out/measure.elf" | tail -20
  printf '%s\n' 'Largest retained compiler-local frames (NOT cumulative stack peaks):'
  head -10 "$out/retained-frames.txt"
  sha256sum "$out/measure.elf" "$out/measure.map"
  printf '%s\n' 'PASS: native self-check; all tiny APIs retained; no unresolved symbols, allocator or I/O syscalls; RAM/flash overflow rejected.'
  printf '%s\n' 'Native harness checks actual discovery/connector begin+request+retries+wipe; full session is a separate reference suite.'
  printf '%s\n' 'NOT target runtime/stack-peak, board-driver, full-DHT-node or side-channel evidence.'
} | tee "$out/report.txt"
