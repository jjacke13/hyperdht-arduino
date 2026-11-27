# Initial component measurements — 2026-10-26

Historical snapshot. For the resumed connector's actual ARM link results see
[R4-MEASUREMENTS.md](R4-MEASUREMENTS.md); for host interoperability see
[CONNECTOR.md](CONNECTOR.md). These old object-only figures are not additive.

This is component evidence, not a working endpoint or a full flash/RAM budget.

## Host checks

Pinned Nix environment: GCC 14.3.0, libsodium 1.0.20, libuv 1.51.0 for the
read-only original-code comparison only. All five suites pass ASan/UBSan:

- RPC/discovery: truncation at every byte, output bounds, hostile lengths,
  actual source/TID matching, deterministic retry/termination caps, malformed
  peer records and bounded candidate exploration.
- UDX receive admission: permutations, sequence wrap, duplicates/reordering,
  fixed-pool startup bursts, explicit release and receive-credit arithmetic.
- Records: fragmented headers/data, incremental RX-pool drain before full-frame
  authentication, pending-message backpressure, coalesced frames, 1 KiB limit,
  bad stream IDs, MAC failures, empty keepalives and state wiping.
- Original-code differential tests: all 64 RPC request/response flag vectors,
  FIND_PEER target/peer-record exchange and bidirectional encrypted records in
  both initiator/responder roles, using all 64 Noise handshake-hash bytes.
- Original UDX sender: actual loopback data, injected ACK/SACK/window feedback,
  real RTO callback and retransmission at MTU 1,200 and 1,500. The old zero-credit
  policy stalls the queued gap; corrected one-free-slot credit delivers it and
  completes the stock write. This controls timer firing and MTU state; it is
  not a normal end-to-end connection or general timer/lost-feedback proof.

The first crypto comparison exposed an incorrect 32-byte hash assumption;
the API and derivation were corrected to the original 64-byte hash before the
passing run. Tiny-to-tiny round trips alone had not detected that mismatch.

No tests in this snapshot contact the public DHT or a physical board. Original
codec/crypto tests are in-process comparisons, not a complete peer connection.

## Cortex-M4 objects

Reproduce inside `nix develop` with `bash scripts/measure-arm.sh`. ARM GCC
14.3.Rel1, `-mcpu=cortex-m4 -mthumb -Os`, C99. These objects cross-compile
against the selected headers but do not link target crypto/libc/drivers.

| Object | Text/constant bytes | Static global data/BSS |
|---|---:|---:|
| rpc.o | 1,510 | 0 |
| discovery.o | 838 | 0 |
| udx_rx.o | 348 | 0 |
| record.o | 802 | 0 |
| Sum | 3,498 | 0 |

These are unlinked objects, not firmware sizes. Zero BSS means callers own the
state; it does not mean zero runtime RAM. Compiler-reported local stack frames
range up to 120 bytes here, excluding callees, crypto, interrupts and board code.

| Caller-owned state, target ABI | Bytes |
|---|---:|
| Discovery workspace | 280 |
| Four-slot UDX receive pool | 5,848 |
| Bounded encrypted-record state | 2,256 |
| Sum if all three coexist | 8,384 |
| One separate maximum encrypted transmit record | 1,044 |
| Subtotal including that TX record | 9,428 |

Do not call the 10 KiB core ceiling closed: this subtotal excludes Noise
handshake state, TX packet metadata/queues, stream/ACK/timer state, UDP/platform
buffers, listener announcement state, crypto stack and application memory.
Some phase storage may be reusable, but overlap/lifetime proof is still needed.
RA4M1 linked image size, target crypto implementation, secure RNG adapter,
network driver and peak call stack remain unresolved.

## Remaining transport gate

The initial receive-credit policy needed correction: advertising zero with one
free gap slot can block stock UDX retransmission. The implementation now keeps
a full-packet grant while one slot remains, with admission reserving that last
slot for the gap. The focused actual-sender regression above passes, including
an old-policy negative control. Complete packet parsing, feedback ordering,
timers and lost-update recovery still need integration; this does not establish
full UDX reliability or interoperability.

Only the reference-sender test relaxes pedantic GNU-syntax checks and preserves
upstream flexible-buffer `stream_send` compiler warnings without treating them
as errors. The tiny C99 library remains under strict warnings. That unordered
send helper is not exercised by this ordered-write regression.

Connector/listener entry points, Noise handshake, full UDX parser/transmission/
timer/close behavior, announcements and optional NAT traversal are still absent.
