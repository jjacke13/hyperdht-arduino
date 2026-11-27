# Cortex-M4 execution surrogate — 2026-11-22

The frozen tiny core and target-built libsodium now execute on QEMU's
`mps2-an386` Cortex-M4. A UART byte bridge carries its actual UDP datagrams to
an actual native HyperDHT server in a loopback-only network namespace. The guest
completes authenticated Noise, encrypted header exchange, a checked 1,024-byte
echo and graceful bidirectional EOF. This is CPU/protocol execution evidence,
**not RA4M1 peripherals, board networking, timing or worst-case stack proof**.

## Build and launch

The ARM build uses the existing pinned Nix shell. Nothing in the root CMake or
flake is changed by this platform patch:

```sh
nix develop --no-write-lock-file
./scripts/run-emulated-m4.sh build instrumented
./scripts/run-emulated-m4.sh build plain
```

Outputs are `build-emulated-m4/{instrumented,plain}/guest.elf`, `guest.map`,
`report.txt`, section/symbol inventories and source manifests. Each variant has
its own cached target crypto archive. A changed compiler/source/flag stamp
moves the old archive aside rather than deleting it. Reports start INCOMPLETE
and become successful only after link checks pass.

The companion host-test patch supplies Python/QEMU and owns the native-peer
`--serve` mode and UDP bridge. With that patch available, a session is run as:

```sh
bash tests/run-loopback-only.sh python3 tests/emulated_bridge.py \
  --elf "$PWD/build-emulated-m4/instrumented/guest.elf" \
  --peer "$PWD/build/test_full_node" \
  --qemu "$(command -v qemu-system-arm)" \
  --output "$PWD/build-emulated-m4/instrumented/session"
```

For UART-only manual launch use:

```sh
./scripts/run-emulated-m4.sh run /tmp/tiny-m4-uart instrumented
```

This launches `-M mps2-an386 -nographic -monitor none -serial unix:PATH,server=on,wait=on
-nic none -kernel ELF`. UART0 carries frames only. The host must capture QEMU
stderr separately. QEMU's model still contains a disconnected Ethernet device;
no backend is attached and the guest MPU does not permit Ethernet MMIO.

There is no unsolicited boot frame or required host startup sleep. The guest
sends terminal RESULT, drains UART TX and waits. The host then terminates/reaps
**its own QEMU process**, independently requiring the native peer to exit
naturally with code zero. No semihosting or guest-controlled host operations
are needed. Missing/truncated frames, exceptions, timeouts and nonzero RESULT
are failures, never implicit success.

## Wire contract

A frame is `[u16LE body length][body]`; length includes the type byte, not the
length prefix. All numeric fields are little-endian; IPv4 fields are four raw
network-order address octets. Exactly one reply follows each input:

| Type | Body | Body bytes |
| --- | --- | ---: |
| 1 CONFIG | type, IPv4, port u16, selected peer public key 32B | 39 |
| 2 STEP | type, monotonic milliseconds u64, source IPv4, source port u16, optional datagram | 15–2063 |
| 3 OUTPUT | type, destination IPv4, port u16, optional datagram | 7–2055 |
| 4 RESULT | type, status u32, observed stack bytes u32, minimum sampled MSP u32 | 13 |

CONFIG is accepted once and acknowledged with an empty OUTPUT whose address and
port are zero. A tick-only STEP has no datagram; its source fields are ignored.
A STEP performs receive, application processing and poll, returning at most one
outgoing datagram. An empty OUTPUT has zero destination fields. Bad framing,
state order or backward time produces terminal failure.

The guest writes exactly one 1,024-byte record, byte `i = i & 255`, validates
one matching authenticated echo and releases it, then queues local END once
outstanding TX has been acknowledged. Success requires verified echo, remote
EOF and local END acknowledgment. Any final outgoing ACK is first offered as
OUTPUT; RESULT is emitted on a subsequent tick with no datagram to send.

Status zero is success. Failure codes are 1 framing/type/length, 2 configuration
or state order, 3 crypto/connector initialization, 4 connector failure/timeout,
5 echo mismatch/extra record, 6 backward time, 7 invalid measured stack bounds,
8 assertion/abort. CPU faults halt; the host deadline then fails the run.

## Memory restrictions and measurement

The link permits 240 KiB of code and 32 KiB of SRAM. QEMU boots at code address
zero; this is not the RA4M1 bootloader address contract. Runtime MPU regions
permit only those memory ranges, UART0 and CPU system registers. The final
16 KiB of a 256 KiB code region are denied, enforcing the 240 KiB limit. SRAM
is non-executable and code is read-only. No privileged background MPU region
is enabled, so the model's additional RAM is not an available resource.

Within SRAM, static storage must fit the first 16 KiB. The next 8 KiB are an
MPU no-access guard, followed by an 8 KiB stack at `0x20006000..0x20008000`.
The large guard catches jumps from large crypto frames, rather than relying
only on a small canary. Its reservation is test overhead, not application RAM.
There is one actual connector, one 2,063-byte shared UART/datagram buffer and
small monitor/RNG state. No guest reference peer, duplicate transport pool,
second record state, heap or guest network stack is allocated.

Assembly startup paints the stack before making any C calls. The plain image
reports touched-stack high water plus a shallow boundary MSP sample. Its MSP
sample is **not** the deepest crypto stack pointer. The instrumented variant
compiles both the actual tiny C core and all target sodium objects with
`-finstrument-functions`. Naked assembly hooks sample MSP after function
prologues without allocating their own frames. The reported observed stack is
the larger of painted-stack high water and `stack_top - minimum_sampled_MSP`.

Newlib assembly and uninstrumented guest monitor helpers are not sampled at
every instruction; touched stack still observes their writes. Unwritten stack
reservations and paint-value coincidences limit plain watermarks. Observed
figures apply to these sessions, not every input, call chain, interrupt pattern
or firmware integration. No production stack-size recommendation is inferred.

The emulator has a compile-time-guarded, explicitly insecure deterministic test
RNG in `platform/emulated-m4/guest.c`. It must never be used for real identities
or firmware. Production `platform/ra4m1/entropy.c` remains unchanged and fails
closed. The protocol core and headers are unchanged from `d908e3b`.

## Measured results

Both variants use the same 43 retained tiny APIs, ARM GCC 14.3.Rel1, Cortex-M4
Thumb soft-float `-Os`, libsodium 1.0.20 full API and no LTO. Source NAR hash:
`sha256-6DC9JJLUwAoZHrYdCIiqOtAlWOEczec2r2FmaBbsa/Q=`. Instrumentation is applied
at the actual archive build, after uninstrumented configure feature probes.
Object disassembly confirms crypto hook calls, including Ed25519; this is not
merely instrumentation around the outer application.

| Quantity | Plain | Instrumented |
| --- | ---: | ---: |
| Code load bytes including `.data` | 110,128 | 113,760 |
| Static RAM: `.data` 1,128 + `.bss` 11,424 | 12,552 | 12,552 |
| Stack reservation | 8,192 | 8,192 |
| No-access guard reservation | 8,192 | 8,192 |
| Static + stack + guard | 28,936 | 28,936 |
| Unassigned SRAM outside those reservations | 3,832 | 3,832 |
| Observed session stack | 6,576 | 6,624 |
| Minimum sampled MSP | `0x20007f78` (boundary only) | `0x20006620` |
| Reservation minus observed stack | 1,616 | 1,568 |

Instrumentation adds 3,632 code bytes and no static RAM here. The observed
stack metrics differ by 48 bytes in these exercised flows; that is not a
worst-case instrumentation-cost bound. The full reserved stack and guard remain
counted regardless of the observed usage.

Frozen ELF/map SHA-256 (debug paths make these checkout-dependent):

| Variant | ELF | Map |
| --- | --- | --- |
| Plain | `9b7bab6e685fefc56d188f51f549f2ffd28145c191139513fc0a647bee53af3e` | `0f28f21043e14ddcd30af379c8078c1181b4a0210ccace0f3d6157d2277ec369` |
| Instrumented | `9817b2af1612bf53b495d79fd8922c38ce6bdb38f082d15e5063b32023eaa18c` | `5efb100845eea8c26838bd14afbc107da4baa412d0ff45f5a0d3cab3b6f33849` |

## Evidence and limits

- Both links pass: ARM executable, no unresolved symbols/interpreter, all APIs
  retained, no selected allocator/syscall symbols. Core manifests match d908e3b;
  production platform and prior measurement script are unchanged.
- Seven real ARM smoke cases pass for **both** variants: unknown type, empty and
  oversized frames, STEP before CONFIG, short CONFIG, backward clock and actual
  Noise request generation/retransmission/exhaustion. Run with Python 3:
  `python3 platform/emulated-m4/smoke.py --elf build-emulated-m4/instrumented/guest.elf`.
  These execute crypto but do not claim a completed authenticated peer session.
- The protocol owner ran the instrumented full native-peer session successfully
  with QEMU 10.1.5; retained PASS run `build/emulated-m4-runs/run-vy0zqk9c`.
  It records 6,624 bytes and MSP `0x20006620`. The Release peer/plain guest PASS
  is `build-release/emulated-m4-runs/run-myfghz77`, recording 6,576 bytes.
  These artifacts were inspected; their host-suite runs were not replayed here.
- Independently executed the plain full session with QEMU 11.1.2 and the provided
  ASan/UBSan native-peer executable in a fresh loopback-only namespace:
  `build-emulated-m4/plain/session/run-v74araio/result.json` is PASS, 7 steps,
  5 guest/6 peer datagrams, observed 6,576 bytes, minimum boundary MSP
  `0x20007f78`. Native peer SHA-256
  `a221c5356e9b1cf0a003091adb104db8e0f8bd59f8e24541e29450c0878cae93`.
  QEMU stderr, stdout and peer stderr are captured separately in that directory.
- QEMU 11.1.2 was also used for both local smoke profiles. No startup sleep is
  needed: a guarded empty UART DATA read after enabling RX re-arms QEMU's socket
  input. Initial pre-fix ELF `26e0e15e` UART stalls are retained separately;
  an early diagnostic reached RESULT zero but its host runner incorrectly
  demanded guest exit. Those FAIL records are not counted as passing runs.
- `bash -n`, source diff checks and source manifests pass. No physical board,
  RA4M1 driver/timing, public networking, lab, deployment or push was involved.
  No Arduino/ESP32 package was modified or built.

QEMU's UART and memory mapping were verified in its source and actual monitor
memory map. MPU field encodings follow the Arm CMSIS Armv7-M definitions:
[QEMU MPS2 model](https://github.com/qemu/qemu/blob/v11.1.0/hw/arm/mps2.c),
[CMSDK UART model](https://github.com/qemu/qemu/blob/v11.1.0/hw/char/cmsdk-apb-uart.c),
[Arm MPU definitions](https://github.com/ARM-software/CMSIS_5/blob/develop/CMSIS/Core/Include/mpu_armv7.h).
