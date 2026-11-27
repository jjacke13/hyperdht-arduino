# hyperdht-tiny

Design for a bounded HyperDHT-compatible endpoint on small microcontrollers.
The primary feasibility target is the Uno R4's Renesas RA4M1: 256 KiB flash,
32 KiB SRAM. An ESP32-S3 build would be a development target, not evidence of
RA4M1 memory fit.

Separate connector and listener APIs would share a bounded protocol core.
Applications could select either role, with NAT traversal in an optional
library. No DHT storage service, maintained routing table, buckets or public-key
allowlist. Exact peer authentication remains required. The listener would
announce its own presence to external DHT nodes.

**Status: resumed (2026-11-22).** Fixed-buffer RPC/discovery, a fixed UDX
receive pool and bounded encrypted records have host sanitizer tests. Wire
tests compare RPC, Noise IK transcripts and encrypted records with the original implementations.
A direct connector now completes discovery and encrypted exchange against an
original-component fixture, and authenticates/exchanges data with an actual
hyperdht-cpp server in an isolated loopback network namespace. The
[Uno R4 WiFi echo example](examples/uno-r4-echo/) also runs on the RA4M1.
It completed public-key lookup and encrypted echo with remote and LAN
JavaScript peers. Listener support remains open.
See [connector scope](docs/CONNECTOR.md) and [measurements and limits](docs/MEASUREMENTS.md).

The [Arduino library preview](ARDUINO-CLI.md) packages the outbound client
and an echo sketch for Uno R4 WiFi. It includes a build helper for the
required linker and WiFiS3 settings.

See [the design and feasibility gates](docs/DESIGN.md).

This is separate from the existing ESP32 apps and Arduino-framework component.
Selected protocol code is adapted with [provenance](PROVENANCE.md) and original
licenses. Existing projects are not modified. No remote is set.

## Host checks with Nix

```sh
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DHDT_REFERENCE_DIR=/path/to/read-only/hyperdht-cpp
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The reference checkout must match PROVENANCE.md for the recorded comparison.
Omit `HDT_REFERENCE_DIR` for self-contained component tests. The reference UDX
sender test uses ephemeral loopback UDP only; no test contacts the public DHT.
Host checks link host libsodium. The separate R4 measurement harness cross-links
target libsodium and the concrete connector; see [R4 link evidence](docs/R4-MEASUREMENTS.md).
The Uno R4 WiFi echo example records the physical board's RAM measurements.

For the optional full native-node test, add `-DHDT_FULL_NODE_REFERENCE=ON`.
It requires Linux user/network namespaces plus `unshare` and `ip`; its child
has only loopback so the reference announcer cannot contact public bootstraps.
Eleven suites pass ASan/UBSan and Release with this option enabled.

For Cortex-M4 object and state-layout measurements, run
`bash scripts/measure-arm.sh` in the same shell. These are not linked firmware
sizes and do not include a target crypto provider or board networking code.
For the current connector plus target crypto link, use `bash scripts/measure-r4.sh`.
It produces a measurement ELF, not a flashable board application.

## Cortex-M4 emulation

The QEMU guest runs the actual connector and target libsodium with 32 KiB SRAM
and a 240 KiB code cap. Its UART bridge reaches an actual native HyperDHT server
inside a loopback-only namespace, checks an authenticated 1 KiB echo and graceful
EOF, and records exercised stack use. This is a CPU surrogate, not an R4 board
or peripheral simulator. See [the contract and limits](docs/EMULATED-M4.md).

The exercised session reports 6,624 bytes of stack with function-entry probes
through the core and crypto, and 6,576 bytes by the plain image's watermark.
The plain image loads 110,128 code/data bytes and allocates 12,552 static RAM
bytes. These results exclude board drivers and interrupt load.

```sh
nix develop
bash scripts/run-emulated-m4.sh build instrumented
cmake -S . -B build -G Ninja -DHDT_FULL_NODE_REFERENCE=ON \
  -DHDT_REFERENCE_DIR=/path/to/read-only/hyperdht-cpp \
  -DHDT_EMULATED_M4_ELF="$PWD/build-emulated-m4/instrumented/guest.elf"
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Omit `HDT_EMULATED_M4_ELF` to keep ordinary host tests independent of QEMU.
The guest's deterministic test RNG is isolated from the production platform;
the real board entropy hook still fails closed.

An [opt-in RA4M1 SCE5 entropy adapter](docs/R4-ENTROPY.md) now has host contract
and failure-path tests. The [Uno R4 WiFi SDK link check](docs/R4-SDK-LINK.md)
measures the SCE5 and WiFiS3 driver costs. Board runtime and packet exchange
remain pending.
