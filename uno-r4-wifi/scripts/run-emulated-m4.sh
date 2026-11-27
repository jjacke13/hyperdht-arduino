#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
action=${1:-build}
variant=${2:-instrumented}
if [[ $action == run ]]; then
  socket=${2:?run requires an absolute UART socket path}
  variant=${3:-instrumented}
fi
case "$variant" in plain|instrumented) ;; *) echo 'Expected plain or instrumented' >&2; exit 2;; esac
out=$PWD/build-emulated-m4/$variant
if [[ $action == run ]]; then
  test -f "$out/guest.elf"
  exec "${QEMU_SYSTEM_ARM:-qemu-system-arm}" -M mps2-an386 -nographic -monitor none \
    -serial "unix:$socket,server=on,wait=on" -nic none -kernel "$out/guest.elf"
fi
[[ $action == build ]] || { echo 'Usage: run-emulated-m4.sh build [plain|instrumented] | run SOCKET [plain|instrumented]' >&2; exit 2; }
mkdir -p "$out"
printf '%s\n' INCOMPLETE > "$out/report.txt"
sodium_source=$(nix eval --raw --impure --expr \
  '(import (builtins.getFlake (toString ./.)).inputs.nixpkgs {system=builtins.currentSystem;}).libsodium.src.outPath')
if [[ ! -d $sodium_source ]]; then
  nix build --no-link --impure --expr \
    '(import (builtins.getFlake (toString ./.)).inputs.nixpkgs {system=builtins.currentSystem;}).libsodium.src'
fi
flags=(-Os -g -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
  -ffunction-sections -fdata-sections -fstack-usage)
instrument=()
[[ $variant != instrumented ]] || instrument=(-finstrument-functions)
sodium_flags="${flags[*]} ${instrument[*]} -DRANDOMBYTES_CUSTOM_IMPLEMENTATION -DRANDOMBYTES_DEFAULT_IMPLEMENTATION=NULL"
configure_flags="${flags[*]} -DRANDOMBYTES_CUSTOM_IMPLEMENTATION -DRANDOMBYTES_DEFAULT_IMPLEMENTATION=NULL"
stamp=$(printf '%s\n' "$sodium_source" "$sodium_flags" "$(arm-none-eabi-gcc --version | head -1)" | sha256sum)
if [[ ! -f $out/sodium-stamp ]] || [[ $(cat "$out/sodium-stamp") != "$stamp" ]]; then
  if [[ -d $out/sodium ]]; then mv "$out/sodium" "$out/sodium.previous.$(date +%s%N)"; fi
  cp -R "$sodium_source" "$out/sodium"
  chmod -R u+w "$out/sodium"
  (
    cd "$out/sodium"
    env CC=arm-none-eabi-gcc AR=arm-none-eabi-ar RANLIB=arm-none-eabi-ranlib \
      CFLAGS="$configure_flags" LDFLAGS='--specs=nosys.specs -mcpu=cortex-m4 -mthumb -mfloat-abi=soft' \
      ./configure --host=arm-none-eabi --disable-shared --enable-static \
      --disable-asm --disable-ssp --without-pthreads > "$out/sodium-configure.log" 2>&1
    # Configure links probe executables before our hooks exist; instrument the
    # actual archive through make's CFLAGS override after feature detection.
    make -j4 CFLAGS="$sodium_flags" > "$out/sodium-build.log" 2>&1
  )
  printf '%s\n' "$stamp" > "$out/sodium-stamp"
fi
include=(-Iinclude -I"$out/sodium/src/libsodium/include")
objects=()
sources=()
for unit in rpc discovery udx_rx record noise transport handshake connector; do
  sources+=("src/$unit.c")
  arm-none-eabi-gcc "${flags[@]}" "${instrument[@]}" "${include[@]}" \
    -std=c99 -Wall -Wextra -Werror -pedantic -c "src/$unit.c" -o "$out/$unit.o"
  objects+=("$out/$unit.o")
done
arm-none-eabi-gcc "${flags[@]}" "${include[@]}" -DHDT_EMULATED_M4_TEST_ONLY=1 \
  -std=c99 -Wall -Wextra -Werror -pedantic -c platform/emulated-m4/guest.c -o "$out/guest.o"
arm-none-eabi-gcc "${flags[@]}" -c platform/emulated-m4/startup.S -o "$out/startup.o"
mapfile -t api < <(arm-none-eabi-nm -g --defined-only "${objects[@]}" | awk '$2 == "T" && $3 ~ /^hdt_/ {print $3}')
roots=()
for symbol in "${api[@]}"; do roots+=("-Wl,--undefined=$symbol"); done
arm-none-eabi-gcc "${flags[@]}" -nostartfiles --specs=nosys.specs \
  -T platform/emulated-m4/guest.ld -Wl,--gc-sections,--print-memory-usage \
  "-Wl,-Map=$out/guest.map" "${roots[@]}" "$out/startup.o" "$out/guest.o" \
  "${objects[@]}" "$out/sodium/src/libsodium/.libs/libsodium.a" \
  -o "$out/guest.elf" > "$out/link.log" 2>&1
arm-none-eabi-nm -u "$out/guest.elf" > "$out/undefined.txt"
test ! -s "$out/undefined.txt"
arm-none-eabi-nm -g --defined-only "$out/guest.elf" > "$out/symbols.txt"
for symbol in "${api[@]}"; do grep -q " $symbol\$" "$out/symbols.txt"; done
! grep -Eq ' (malloc|calloc|realloc|sodium_malloc|_sbrk|_read|_write)$' "$out/symbols.txt"
arm-none-eabi-size -A "$out/guest.elf" > "$out/sections.txt"
arm-none-eabi-readelf -h -l -A "$out/guest.elf" > "$out/elf.txt"
grep -q 'Machine:.*ARM' "$out/elf.txt"
! grep -q INTERP "$out/elf.txt"
sha256sum "${sources[@]}" include/hdtiny/*.h src/compact_internal.h > "$out/core-inputs.sha256"
sha256sum platform/emulated-m4/* scripts/run-emulated-m4.sh flake.lock > "$out/platform-inputs.sha256"
{
  printf 'EMULATED M4 TEST ONLY: %s; %d tiny APIs retained\n' "$variant" "${#api[@]}"
  arm-none-eabi-gcc --version | head -1
  printf 'sodium source NAR: '; nix hash path "$sodium_source"
  cat "$out/link.log" "$out/sections.txt"
  sha256sum "$out/guest.elf" "$out/guest.map"
  printf '%s\n' 'Link checks PASS. Execute through UART bridge; no hardware or RA4M1 timing proof.'
} | tee "$out/report.txt"
