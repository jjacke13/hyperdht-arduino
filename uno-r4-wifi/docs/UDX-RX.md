# Fixed UDX DATA receive admission

This is a host C99 receive-sequence/credit component, **not a UDX packet parser
or complete transport**. It implements no socket, authentication, crypto,
listener, discovery, send scheduler, timer, END or DESTROY processing. It does
not establish HyperDHT interoperability or RA4M1 fit. Only the files described
here and the accompanying notices belong to this patch; build integration is
the integrating owner's work.

## Mandatory caller boundary

Before `hdt_udx_rx_admit`, the future packet parser must validate datagram
length, UDX magic/version, flags, extensions/offset arithmetic, bound socket
source address/port, local stream ID and stream lifecycle. Pass only extracted,
nonempty DATA payload bytes and the wire sequence. A combined DATA/END packet
requires an explicit future design; do not silently strip END. ACK/SACK-only,
MESSAGE, HEARTBEAT, END-only and DESTROY packets do not consume a DATA slot
through this API. In particular a padded MTU probe is not application data.
Source migration is unsupported until an authenticated migration policy exists.

This module cannot reject a forged source or stream ID: it never receives
those values. UDP address checks alone are not authentication. Retained stream
bytes must still pass SecretStream authentication before application delivery.
All object/input pointers must be valid, the pool must not be mutated directly,
and calls must be serialized without reentrancy. Payload input must not overlap
the selected destination slot. Initialization discards state and is permitted
only before stream use or after teardown, never during a live stream.

## Pool, sequence and ownership

Four slots each hold up to 1,452 bytes, the maximum IPv4 DATA payload under the
reviewed MTU range 1,200..1,500 with 20-byte UDX header. No heap allocation or
callbacks occur. Nonempty payload length is checked before any copy. Sequence
subtraction uses unsigned modulo-2^32 arithmetic; exactly half-range distance
is dropped as ambiguous. Old sequences are duplicate reports; future sequences
must be within four packet numbers of the cumulative ACK. This narrow reorder
window is a resource policy and can increase retransmissions.

`ack` is the next sequence not cumulatively retained. Admission copies bytes
before advancing it, then walks contiguous retained slots. A dropped packet
never advances ACK or appears in the SACK mask. Old retransmissions may be
reported duplicate after their payload has been consumed/released, since the
receiver has already accepted responsibility for those bytes. Differing
duplicate bytes never replace the first retained bytes. This is transport
sequencing, not integrity authentication.

At least one free slot is reserved for the missing ACK sequence: out-of-order
admission cannot take the last slot. In-order admission may fill the pool.
`peek` borrows the oldest ordered slot without freeing it; `release(seq)` frees
only that oldest slot and advances the read position. Its pointer becomes
invalid on release/teardown. There is no application callback in admission.
The cumulative ACK can advance before dequeue, but ownership remains in the
pool until release. `sack_mask` describes retained `ack+i` sequences in bits
`i`; it is an internal snapshot, **not the UDX wire SACK block encoding**.
Future ACK emission must use current snapshots, not ACK a supplied packet
unconditionally based on its arrival/result enum.

Observed on the tested x86_64 host ABI:

| Storage | Bytes |
|---|---:|
| Four payload arrays | 5,808 |
| Four sequence/length pairs plus slot padding | 32 |
| ACK and read cursors | 8 |
| Complete `sizeof(hdt_udx_rx_t)` | 5,848 |
| Separate maximum ciphertext record with length prefix | 1,044 |
| Separate maximum plaintext output, if not safely overlapped | 1,024 |
| RX pool plus those two future record buffers | 7,916 |

The last three entries are design reservations, not implemented allocations.
Slot layout is 4-byte sequence + 2-byte length + 1,452-byte data + host padding.
Target ABI sizeof, stack, network datagram buffers, TX lifetime, crypto state,
discovery state and platform storage remain to be measured. Of a 10KiB core
ceiling only 2,324 bytes remain after that 7,916-byte reservation; **the whole
core budget is not closed**. No 1,452-byte automatic array exists in the module;
tests intentionally have larger host-only objects.

## Credit and forward progress

The reviewed libudx sender rounds receive credit down to a packet count:
`floor(rwnd / (MTU - 48))` for IPv4, then limits by its congestion window.
A 1,044-byte record buffer alone therefore cannot define advertised credit:
that credit is smaller than both 1,152 and 1,452-byte sender payload units.

This component advertises **1,452 bytes when at least one slot is free,
otherwise zero**. At every peer MTU from 1,200 through 1,500, that positive
credit permits exactly one ordinary packet under the reviewed rounding rule.
It never advertises a positive sub-packet residual. One free slot physically
holds the entire 1,452-byte grant, including when that slot is reserved for a
missing sequence. Non-gap out-of-order packets still cannot take the last
slot. This is deliberately conservative, not a throughput optimization.
Outside that peer MTU range the proof does not hold; a local transmit-MTU cap
does not cap the peer's MTU.

ACK/window advertisement is not a hard admission limit. The stock sender
starts with 4MiB credit and congestion window 10 before receiving updates.
Old credit, bursts, small packets and retransmission/probes can overrun any
current grant. The fixed pool and sequence checks independently bound them:
excess DATA is dropped without acknowledgment of those bytes. The missing
sequence retains a reserved slot even if out-of-order traffic arrives first.
Startup loss requires ordinary peer retransmission; bounded storage does not
guarantee survival of arbitrary loss or a peer's finite retry timeout.

Advertising zero with a reserved slot was incorrect: the reviewed stock
`send_packets` gates even retransmission on `stream_may_send`, which requires
a positive packet-rounded window. RTO merely queues the missing packet and
calls that same sender. Zero-window probing tries new unsent data rather than
that retransmit queue; it cannot guarantee recovery when all writes are sent.
The one-free-slot grant keeps gap retransmission eligible.

After a full pool, releasing one slot reopens 1,452 bytes. A full pool can
still contain later gaps/OOO data, but the last accepted packet was in-order,
so some ordered data is available to consume/release. A future integration
**must send a fresh ACK/window
update after release, including while no packet arrives**, and implement the
UDX zero-window/probe/retransmission rules and window-update ordering. This
module produces values and never sends packets. It does not prove the future
sender/timer integration or recovery from a lost window update.

The frame consumer must copy ordered ciphertext into a separate bounded frame
accumulator and release raw packet slots incrementally, even before the whole
frame/tag is available. Waiting for full authentication before freeing any
raw slots deadlocks on a record fragmented into more than four small packets.
Do not expose unauthenticated plaintext. The cooperating application must
limit each encrypted application write to 1,024 plaintext bytes: 1,041 encrypted
body bytes plus 3-byte prefix. Handle the initial 56-byte header separately.
The future accumulator must reject a declared oversized body as soon as its
length is known and bound coalesced/multiple-frame input; it is not implemented
here. On parser/auth failure, terminate rather than reset a live ACK cursor.

## Adaptation and license

Source read-only: `hyperdht-cpp/deps/libudx`, clean revision
`759bf7641347f65a1896d4bc4c3b1606a459f466` (2026-10-26 review).
Upstream project: <https://github.com/holepunchto/libudx>.

Selected adaptations:

| libudx source | Reused semantics and change |
|---|---|
| `src/internal.h`, `seq_diff` | Wrap-aware subtraction; use unsigned half-range checks instead of implementation-defined unsigned-to-signed conversion |
| `src/udx.c`, `process_data_packet` and contiguous read-queue drain | Sequence-unit ACK advance and reorder drain; copy into fixed slots before acknowledging, with explicit consumer release |
| `include/udx.h`, header sizes and MTU constants | IPv4 payload ceiling; fixed constants instead of the full dependency |
| `src/udx.c`, `send_rwnd_in_packets`, `send_window_in_packets`, `get_recv_rwnd` | Basis for credit arithmetic; replace general byte accounting with conservative fixed-slot admission credit |

This is a rewritten bounded adaptation of selected algorithms/constants, not
a vendored libudx implementation. It reuses no crypto code. libudx attribution
is retained in `src/udx_rx.c`, its exact upstream NOTICE is copied to
`licenses/libudx-NOTICE.txt`, and its Apache-2.0 license is copied to
`licenses/Apache-2.0.txt`. The new module, header and tests carry Apache-2.0
SPDX identifiers; modified-source notice is in the implementation. No BBR or
Google window-filter code was copied. No existing owner's source was edited.

## Host verification

`tests/test_udx_rx.c` exports `int hdt_test_udx_rx(void)` for integration, with
an optional standalone main selected by `HDT_UDX_RX_TEST_MAIN`. Assertions stay
enabled in this test translation unit even when the root build sets NDEBUG.

```sh
cc -std=c99 -Wall -Wextra -Werror -pedantic -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DHDT_UDX_RX_TEST_MAIN -Iinclude \
  src/udx_rx.c tests/test_udx_rx.c -o /tmp/test-udx-rx
/tmp/test-udx-rx
```

Tests cover all 24 four-packet arrival permutations before and across sequence
wrap, gap reservation/retransmission, duplicate payload preservation, SACK
snapshots, invalid lengths/half-range sequences, explicit dequeue ordering,
maximum payload, a 10,000-packet ignored-credit startup burst, zero-to-positive
credit recovery, peer packet-rounded credit at MTUs 1,200/1,500, and complete
1,044-byte record copying with fragments of 1, 17, 522 and 1,044 bytes. These
are receive component and arithmetic tests, not encrypted-record/authentication
tests. No target build or hardware test was run.

### Original-sender regression

`tests/test_udx_rx_sender.c` includes the pinned original `src/udx.c` through
`HDT_LIBUDX_SOURCE`; do not also compile/link another copy of `udx.c` or libudx.
It uses the real private `process_packet`, `udx_rto_timeout`, `send_packets`
and retransmission implementation without rewriting or stubbing them. The
sender's data datagrams travel through real IPv4 loopback UDP sockets bound
to ephemeral ports. ACK/SACK/window feedback is injected into the real packet
parser in memory. Each MTU case:

1. Sends exactly four full-payload packets using ordinary stock write calls;
   drops the first receive and retains sequences 1..3 in the RX pool.
2. Parses a SACK block [1,4) with cumulative ACK 0 and the old zero credit;
   verifies those three sender packets are actually SACKed/freed and that no
   unsent write data remains.
3. Invokes the real RTO callback directly, proving the earliest loss is queued
   but cannot retransmit at zero credit (negative control).
4. Parses the corrected 1,452-byte window update, causing stock `send_packets`
   to transmit the queued RTO loss. Receives sequence 0 over UDP, advances RX
   ACK to 4 and parses the cumulative feedback to complete the original write.

Both MTUs 1,200 and 1,500 pass this regression under ASan/UBSan. The test pins
MTU state and directly invokes RTO to avoid waiting for wall-clock timers.
It proves this specific sender-callpath recovery, not timer scheduling,
MTU discovery, lost-feedback recovery, a complete tiny transport, or normal
HyperDHT/SecretStream interoperability.

Dependencies: a C compiler with GNU C11 support for original libudx,
libuv 1.51.0 headers/library, Linux/POSIX UDP, libm, and the read-only pinned
libudx source tree. The core and standalone component test remain strict C99.
The included original source and separately linked support files retain their
upstream licenses and notices in place; no dependency source is copied here.
Do not build the reference test for embedded targets. From this clone:

```sh
udx_source=/home/jacke/Desktop/repos/hyperdht-cpp/deps/libudx
# Supply libuv's include/library search flags through the host environment.
cc -std=gnu11 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DHDT_LIBUDX_SOURCE="\"$udx_source/src/udx.c\"" -Iinclude \
  tests/test_udx_rx_sender.c src/udx_rx.c \
  "$udx_source/src/cirbuf.c" "$udx_source/src/endian.c" \
  "$udx_source/src/queue.c" "$udx_source/src/udx_rate.c" \
  "$udx_source/src/win_filter.c" "$udx_source/src/win_filter_f64.c" \
  "$udx_source/src/udx_bbr.c" "$udx_source/src/io_posix.c" \
  -luv -lm -o /tmp/test-udx-rx-sender
/tmp/test-udx-rx-sender
```
