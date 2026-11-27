# RA4M1 connector cross-link measurement — 2026-11-22

This is a **final linked ARM ELF/map containing the actual direct connector**,
not a flashable UNO R4 application or successful R4 hardware connection. It
includes discovery, handshake codecs, Noise, record crypto and transport. The
platform still has no network driver and its entropy hook deliberately fails
closed. Do not flash this ELF.

## Reproduce

Use the repository's unchanged pinned Nix shell:

```sh
nix develop --no-write-lock-file
./scripts/measure-r4.sh
```

The script compiles all eight tiny source units and target libsodium, retains
all exported tiny APIs, and links `build-r4/measure.elf` plus `measure.map`.
It also emits `report.txt`, `inputs.sha256`, section/symbol/ELF inventories,
`layout.txt`, retained compiler-local stack frames and negative-link logs.
The report starts as INCOMPLETE and is replaced by success only after all checks.

This revision requires the actual connector sources. The earlier optional Noise
snapshot override is removed: silently measuring a partial profile would undercount
this checkpoint. Existing differently configured crypto builds are moved aside,
not deleted. No hardware is accessed or flash binary generated.

## Source and toolchain pins

- Clean integration base when cloned:
  `cbd221d30be0a4217111a82ad1b9f6ecd8ef53a5`.
- Alberto's original measurement commit: `3a30cd9dd33461c698014e2eef7cfe6650f5abff`.
- Dave's six incremental patches through connector checkpoint
  `7886f9b9cf2f84ea75724da9919e6b3d7f10c411` were applied **only to Alberto's
  isolated measurement checkout**. They include Noise, transport, the combined
  END backlog fix, connector/handshake glue, stale window ordering fix
  `30e84ff59438c7846ffa98aee59c092ca37e910f` and remote half-close reply fix.
  Shared integration was untouched.
- Alberto's incremental measurement change touches only the reserved platform,
  script and this document. Root CMake/flake changes came from Dave's patches.
- nixpkgs remains `b6018f87da91d19d0ab4cf979885689b469cdd41`.
- ARM GNU Toolchain **14.3.Rel1**, GCC `14.3.1 20250623`, Cortex-M4 Thumb,
  soft-float ABI, `-Os`, separate code/data sections, **no LTO**.
- libsodium **1.0.20**, source NAR hash
  `sha256-6DC9JJLUwAoZHrYdCIiqOtAlWOEczec2r2FmaBbsa/Q=`.
  Full API, portable implementations (`--disable-asm`), static build, no pthreads
  or default OS RNG. `--enable-minimal` excludes the Ed25519 noclamp primitive
  this handshake needs and cannot be used unchanged. No upstream crypto code
  was edited.
- Host reference sources independently verified at
  hyperdht-cpp `53c9bb52fe9eea6dedea2ad87b29fcabb576a270` and
  libudx `759bf7641347f65a1896d4bc4c3b1606a459f466`; consumed paths were clean.
  Host GCC 14.3.0, libsodium 1.0.20 and libuv 1.51.0.

## Actual final link

| Quantity | Direct connector harness |
| --- | ---: |
| Retained tiny API functions | 43 |
| Flash load bytes, including initialized data | 109,536 |
| Available application flash | 245,760 |
| Remaining application flash | 136,224 |
| `.vectors` | 64 |
| `.text` including rodata | 108,336 |
| `.ARM.exidx` | 8 |
| `.data` | 1,128 |
| `.bss` | 11,496 |
| Total static RAM | 12,624 |
| Explicit stack reservation, unmeasured peak | 8,192 |
| Static RAM + reserved stack | 20,816 |
| Unassigned physical SRAM | 11,952 |
| Harness state within `.bss` | 11,392 |

The linker models SRAM at `0x20000000`, length 32 KiB, and application flash at
`0x4000`, length 240 KiB. The first 16 KiB of physical flash is reserved for the
UNO R4 loader, rather than claiming all 256 KiB for the application. Debug
sections enlarge the ELF file on disk but do not consume target flash.

The stack is placed at the top of SRAM, so ld's region summary says RAM 100%.
The table counts static sections plus the explicit reservation and excludes the
unused gap. That gap is not an implemented heap or a guaranteed application budget.

Target ABI sizes, measured in an **unlinked sizeof probe**, are:

| Type | Cortex-M4 bytes |
| --- | ---: |
| `hdt_discovery` | 280 |
| `hdt_noise` | 552 |
| `hdt_record` | 2,256 |
| `hdt_transport` | 6,960 |
| `hdt_connector` | 9,248 |

Only one connector is allocated. Its internal union already overlays Noise and
retained request with transport+record; the transport already owns its RX pool
and one 1,044-byte TX buffer. No separate RX, TX, record or handshake fixtures are
added to the target inventory. An outer harness union shares discovery with the
connector: discovery is discarded before begin, as exercised by the smoke path.
The measured 11,392-byte harness state is 9,248 bytes for that phase union,
2,048 bytes for datagram scratch, and 96 bytes for selected key and two seeds.
Identity inputs and packet scratch remain outside the union while begin runs.

Every public tiny API is rooted during linking, even if the smoke path does not
call it. Unrelated sodium functions are garbage-collected normally. Unmodified
`sodium_init()` retains some unused Argon2/AEGIS dispatch targets; their flash,
data and local frame sizes are counted rather than replaced by initialization
stubs. No allocator or selected POSIX I/O syscall survives the final link.

The largest retained compiler-local frame is 4,128 bytes in Argon2
`generate_addresses`; protocol-relevant Ed25519 `ge25519_scalarmult` is 4,048.
Connector begin is 1,416 bytes and receive 824 bytes. These are **individual
frames**, excluding callers/callees, IRQs and future platform code. The 8 KiB
reservation is a budget, not evidence that the whole stack fits.

## Verification actually performed

- `./scripts/measure-r4.sh`: pass. ARM executable/soft-float ELF checked, no
  interpreter or unresolved symbols, all 43 APIs retained, and no selected
  allocator or I/O syscall symbols in the final ELF.
  Final relink used the unchanged cached crypto archive: mtime `1795344387`,
  size 1,875,012 bytes before and after. No sodium rebuild was required.
- The native harness now calls the actual discovery and connector begin/poll
  APIs: request decoding, short-buffer behavior, retry timing, attempt exhaustion
  and complete state wiping pass with host libsodium. It generates no UDP traffic.
  This replaces the old independent record fixture self-check. It is not a full
  session or target-runtime test.
- Deliberate static RAM overflow and application flash overflow both fail with
  the intended linker assertions.
- `bash -n scripts/measure-r4.sh` and `git diff --check`: pass.
- Independently rebuilt the unchanged root tests under ASan/UBSan and ran **10/10**:

```sh
nix develop --no-write-lock-file -c cmake -S . -B build-r4-reference \
  -DHDT_SANITIZE=ON -DHDT_REFERENCE_DIR=/home/jacke/Desktop/repos/hyperdht-cpp
nix develop --no-write-lock-file -c cmake --build build-r4-reference -j4
nix develop --no-write-lock-file -c ctest --test-dir build-r4-reference --output-on-failure
```

The suite covers codec/Noise parity, RX credit, fixed transport and the joined
FIND_PEER -> Noise RPC -> encrypted header -> two 1 KiB records each way ->
backpressure -> EOF flow with dropped requests/DATA and duplicates. Its peer is
assembled from original components, **not a complete production DHT node**.
The updated boundary suites include stale receive-window ordering and a reply
after authenticated headers plus remote FIN. The latter now returns write success.
Dave's Release results remain peer-reported; they were not repeated here.

Newlib prints I/O-stub warnings before section garbage collection. Separately
checked final symbols exclude `_read`, `_write`, `_close`, `_lseek`, `_sbrk`,
`malloc`, `calloc`, `realloc` and `sodium_malloc`. Fatal assertions/abort halt;
validation and assertions remain enabled without stdio/signal machinery.

## Preserved evidence

Current artifacts are frozen under `build-r4/connector-final/`, including ELF/map,
report, source manifest, target layouts, retained frames and overflow logs.
Host build/test logs are `build-r4/final-reference-{build,tests}.log`.
Debug paths make ELF/map hashes dependent on the checkout location.

- ELF SHA-256: `a062c35c1c7c0d47db53047f77d7232218e9487ac404a3940ba26a0996accd89`
- Map SHA-256: `c199c579d9d601cefdddf125ba2f44464fcde21527529aee48050fd7f65b8175`

The previous f7b6af0 connector artifacts remain in `build-r4/connector/`.
The two fixes added 16 flash bytes and 8 static RAM bytes. The measurement script,
platform harness and crypto configuration needed no further code changes.

Historical `build-r4/foundation/` and `build-r4/noise/` artifacts remain preserved.
They measured 99,392/105,280 flash bytes and 12,904/14,576 static RAM bytes with
independent component fixtures. Those RAM inventories are superseded by the
actual connector/phase unions; the historical figures are not additive.

## Bounded connector review and limits

The material half-close issue reported at f7b6af0 is fixed in 7886f9b. Receiving
remote EOF still closes reception, but write now permits READY or EOF while
preserving local-end and TX backpressure guards. The new authenticated-header/
FIN/reply boundary regression passes independently under ASan/UBSan. The older
failing-behavior reproduction remains preserved under `build-r4/review-halfclose*`;
its old assertion deliberately describes the pre-fix checkpoint.

The stale-window fix tracks peer sequence/ACK ordering in two additional 32-bit
fields before accepting credit updates. The new stale-sequence regression passes.
Bounded review of these two changes established no additional material finding.
The joined component-fixture suite still completes its writes before either EOF;
a full real-node request/FIN/reply test is not claimed by this boundary regression.
No core files were edited by the platform measurement work.

The entropy hook intentionally halts until a reviewed board source replaces it.
Startup only initializes data/BSS and calls the harness. Missing platform work
includes the RA4M1 BSP, clocks/watchdog/interrupt routing, bootloader entry contract,
board RNG, real UDP/WiFi runtime and UNO R4 WiFi coprocessor driver. Loader and
option-setting bytes were not written. No target execution, timing, physical
stack peak, hardware, lab, public-peer traffic, deployment or push was performed.
The direct core advertises OPEN and requires actual reachability; NAT/punching,
listener service and full ordinary-DHT-node interoperability remain separate gates.

The actual connector's linked components fit the modeled memory map. **A complete
networked board application fitting and performing acceptably remains unproven.**

Subsequent host-only validation built the actual native HyperDHT library/server
and SecretStreamDuplex in the tiny build directory, with original source unchanged.
The tiny connector authenticated, exchanged a 1KiB encrypted echo and completed
graceful EOF in a loopback-only user/network namespace, under both sanitizers and
Release. This adds a real native-peer direct-connection check beyond the earlier
component fixture; normal discovery/relay topologies and board runtime remain
separate. No target core source changed after the frozen measurement above.

Sources checked for the platform model and crypto build:
[Renesas RA4M1 datasheet](https://www.renesas.com/en/document/dst/ra4m1-group-datasheet?language=en),
[Arduino UNO R4 WiFi map](https://github.com/arduino/ArduinoCore-renesas/blob/main/variants/UNOWIFIR4/memory_regions.ld),
[libsodium ARM guidance](https://doc.libsodium.org/installation),
[custom RNG interface](https://doc.libsodium.org/doc/advanced/custom_rng).
Libsodium cautions that Cortex-M0/M3/M4 use is untested and not recommended where
side channels are a concern. This is link-size evidence, not a side-channel or
production-key qualification.
