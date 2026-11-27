# Uno R4 WiFi SDK link measurements

An Arduino CLI build for `arduino:renesas_uno:unor4wifi` links the fixed-memory
core, ARM libsodium and actual RA4M1 SCE5 driver. The probe reuses the existing
connector measurement program and retains all 43 public core functions. The
baseline measurements use the original board linker script and its 1 KiB main
stack. The `sce5-wifi-stack8k` profile links with an actual 8 KiB reservation.

The `sce5-wifi` profile adds a single `WiFiUDP` instance and retains the
WiFiS3 station and UDP call paths. The retained probe references station
connect/status/disconnect, socket bind/close, send, and receive with source
address and port. It uses the same SDK and core as the entropy builds. The
probe takes a caller-supplied buffer and `setup()` does not call it.

Two baseline builds used the same sources and settings except for selection of
the strong entropy provider. All four ELFs have no undefined symbols. The SDK link
map resolves `HW_SCE_McuSpecificInit` and `HW_SCE_RNG_Read` from the board's
`libfsp.a`. The fallback build contains the weak hook and neither SCE5
entry point. The real SDK compile revealed that including `fsp_common_api.h`
directly introduces a circular include via `fsp_version.h`. The entropy adapter
now starts from `bsp_api.h`. All five focused host cases still pass in both
ASan/UBSan and Release.

| Measurement | Weak fallback | SCE5 selected | Increase |
| --- | ---: | ---: | ---: |
| Application flash span, including alignment and initialized data | 163,172 B | 167,588 B | 4,416 B |
| Static SRAM through `__HeapBase` | 19,384 B | 19,520 B | 136 B |
| Unassigned SRAM after budgeting 8,192 B stack and 256 B RAM vectors | 4,936 B | 4,800 B | |

| Measurement | SCE5 selected | SCE5 and WiFiS3 | WiFiS3 increase |
| --- | ---: | ---: | ---: |
| Application flash span | 167,588 B | 176,312 B | 8,724 B |
| Static SRAM through `__HeapBase` | 19,520 B | 21,800 B | 2,280 B |
| Unassigned SRAM after stack and vectors | 4,800 B | 2,520 B | |

With WiFiS3 linked, the remaining application flash is 69,448 bytes. The one
UDP object occupies 1,496 static bytes, including its 1,461-byte receive FIFO.
The library also accumulates modem replies in a `std::string` during reads.
That transient heap use shares the 2,520-byte allowance with the application
and other runtime allocations. Measuring full datagrams on the board is
necessary to establish the remaining application budget.

The flash span starts at `0x4000` and includes all flash loadable sections
through the last byte. The usable application region is 245,760 bytes. The
static SRAM tally includes alignment below `__HeapBase` and does not count
heap allocations. The stock SDK places the stack between `0x20007b00` and
`0x20007f00`. The compiler reports a 48-byte local frame for the entropy adapter.
That excludes the callback and vendor call chain.

## Wi-Fi probe with an 8 KiB stack

The measurement script creates a private board variant in the build directory.
Its linker layout reserves the full stack below the 256-byte RAM vector table.
The SDK's fixed 8 KiB heap input array would overlap that stack, so the layout
discards that placeholder and reserves the available space up to the stack
instead. The SDK's 1 KiB stack object stays at the top of the reservation
because the flash reset vector points to its end.

| Region | Range (end exclusive) | Size |
| --- | --- | ---: |
| Static RAM and alignment | `0x20000000`–`0x20005528` | 21,800 B |
| Heap for runtime allocations | `0x20005528`–`0x20005f00` | 2,520 B |
| Main stack | `0x20005f00`–`0x20007f00` | 8,192 B |
| RAM interrupt vectors | `0x20007f00`–`0x20008000` | 256 B |

Flash span is 176,328 bytes. The check reads the initial stack pointer and
reset handler directly from the flash vector table. The linked `_sbrk`
references the new heap bounds. The station and UDP calls are retained in the
ELF for measurement. A board run is needed to measure Wi-Fi heap use and peak
stack while joining a network and exchanging UDP packets.

## Reproduce the measurement

Use Arduino CLI with the pinned `arduino:renesas_uno@1.6.0` core and its
`arm-none-eabi-gcc@7-2017q4` toolchain. Python 3 is required for the map check.
The release archives and SHA-256 hashes used here came from the [Arduino
package index](https://downloads.arduino.cc/packages/package_index.json).

| Release archive | SHA-256 |
| --- | --- |
| `ArduinoCore-renesas_uno-1.6.0.tar.bz2` | `71aa5d81f9455dbc2e8977b594963494e41466a8ed439a841b9d029b4aa31c8b` |
| `gcc-arm-none-eabi-7-2017-q4-major-linux64.tar.bz2` | `96a029e2ae130a1210eaa69e309ea40463028eab18ba19c1086e4c2dafe69a6a` |

Build the locked libsodium 1.0.20 source in a writable directory. From the
repository root, set `toolchain` and `sodium_build` to absolute directories.
Use the same Cortex-M4 hard-float ABI as the board recipe.

```sh
sodium_source=$(nix eval --raw --impure --expr \
  '(import (builtins.getFlake (toString ./.)).inputs.nixpkgs {system=builtins.currentSystem;}).libsodium.src.outPath')
cp -R "$sodium_source" "$sodium_build"  # use a new destination
chmod -R u+w "$sodium_build"
(
  cd "$sodium_build"
  tools="$toolchain/bin/arm-none-eabi"
  abi='-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard'
  env CC="$tools-gcc" AR="$tools-ar" RANLIB="$tools-ranlib" \
    CFLAGS="$abi -Os -ffunction-sections -fdata-sections -DRANDOMBYTES_CUSTOM_IMPLEMENTATION -DRANDOMBYTES_DEFAULT_IMPLEMENTATION=NULL" \
    LDFLAGS="$abi --specs=nosys.specs --specs=nano.specs" \
    ./configure --host=arm-none-eabi --disable-shared --enable-static \
      --disable-asm --disable-ssp --without-pthreads
  make -j4
)
bash scripts/measure-r4-sdk.sh "$toolchain" "$sodium_build"
```

The script produces a fresh `build-r4-sdk/link.*` directory with the compile
logs, section tables, link maps, symbol inventories and artifact hashes.
