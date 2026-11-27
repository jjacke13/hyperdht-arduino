# Opt-in UNO R4 entropy provider

This step adds `platform/arduino-r4/entropy.c`, a strong implementation of the
existing `hdt_ra4m1_entropy` hook. It calls the RA4M1 SCE5 random-number APIs;
Noise, record crypto and transport stay on the RA4M1. The provider is not enabled
in the default library, measurement ELF or emulator. No board was accessed.

## Source contract

The checked source is ArduinoCore-renesas **1.6.0**, commit
`424e86eff92d37f72123c2b641dd8bbf06a38b47`, whose `extras/fsp` submodule is
`9b5a853bd1b0040e7bb4350b7cf36c7767c9b4f8`.

- [WMath.cpp](https://github.com/arduino/ArduinoCore-renesas/blob/424e86eff92d37f72123c2b641dd8bbf06a38b47/cores/arduino/WMath.cpp#L37)
  uses `fsp_err_t HW_SCE_McuSpecificInit(void)` and
  `fsp_err_t HW_SCE_RNG_Read(uint32_t *)` for both UNO R4 WiFi and Minima.
  Its public `random()` interface ignores read status, transforms the result
  and switches to `rand()` after `randomSeed()`. The adapter calls the checked
  driver interface directly.
- [The RNG header](https://github.com/arduino/ArduinoCore-renesas/blob/424e86eff92d37f72123c2b641dd8bbf06a38b47/variants/MINIMA/includes/ra/fsp/src/r_sce/hw_sce_trng_private.h#L28)
  specifies a 128-bit result: an aligned array of four `uint32_t` values.
- [The actual FSP adapter](https://github.com/arduino/fsp/blob/9b5a853bd1b0040e7bb4350b7cf36c7767c9b4f8/ra/fsp/src/r_sce/crypto_procedures/src/sce5/plainkey/adaptors/r_sce_adapt.c#L102)
  powers/resets/self-checks the engine, sets little-endian mode and propagates
  generator failure through the read result.
- [Renesas documents SCE5/TRNG on RA4M1](https://www.renesas.com/en/document/dst/ra4m1-group-datasheet).
  A hardware feature declaration does not qualify this integration's entropy.

No Arduino or Renesas implementation is vendored. This is new integration glue
calling their existing private API; vendor headers and drivers retain their own
licenses. The private interface requires revalidation when the core/FSP changes.

## Ownership and failure behavior

The application must serialize calls from main context and give this adapter
exclusive ownership of SCE initialization and use. Do not combine it with
Arduino `random()`, other SCE clients, interrupt callers or concurrent tasks.
Initialization resets the engine, so it happens once on the first nonempty
request, rather than once per block. No deinitialization, recovery or automatic
registration is provided.

Each request reads aligned 16-byte blocks and copies only the requested bytes;
unused tail bytes are discarded. It retains only the initialization/failure
state, never leftover random bytes. Temporary blocks are wiped with
`sodium_memzero`. Any reported init/read failure wipes the *whole* requested
output, including bytes copied before a later block failed, and latches failure
until device reset. There are no retries or alternative entropy sources.

A zero-length request succeeds without touching hardware. A null pointer with
nonzero size fails without initializing hardware. All other buffers must be
valid for their requested size. On success the caller owns the random bytes.

To select this provider in a later board build, compile it with the actual UNO
R4 core's headers and board define, alongside the existing
`platform/ra4m1/entropy.c` and target libsodium. Include the adjacent `entropy.h`
and call `hdt_ra4m1_install_entropy()` before `sodium_init()` or any random-byte
request. The new strong hook replaces the existing weak fallback; the existing
libsodium callback still wipes and halts on failure. No source is selected merely
by including the header. Both UNO R4 board defines are admitted; no network
interface or board variant was chosen by this step.

## Checks and remaining gate

The host test target compiles the actual adapter with a tiny fake-FSP header and
driver. Its five independent process cases cover successful one-time init;
unaligned outputs and 1/15/16/17/31/32/33-byte bounds; null/zero requests; init
failure with no read; first-read failure; later-read failure with complete output
wiping; and a sticky failure with no subsequent hardware calls. Fixture bytes
are deterministic test data and do not enter any production random callback.

Run in the pinned Nix shell after configuring the normal host build:

```sh
cmake --build build --target test_r4_entropy
ctest --test-dir build -R '^r4_entropy_' --output-on-failure
```

These host tests establish adapter and error-handling behavior. The separate
[Uno R4 WiFi SDK measurement](R4-SDK-LINK.md) verifies real HAL linkage and
measures the linked flash and static RAM cost. The SDK probe still has the
stock 1 KiB stack reservation. Hardware self-checks, initialization time,
reseed and health behavior and entropy quality still require board testing.
The vendor routines may wait internally. The adapter adds no hardware timeout.
