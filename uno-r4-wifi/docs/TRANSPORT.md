# Bounded direct UDP stream transport

`hdt_transport` adds a cooperative poll adapter around the four-slot RX pool
and one 1,044-byte outstanding TX buffer. The caller supplies the negotiated
peer IPv4 address, UDX stream IDs and starting sequences. It owns the UDP
socket, sends returned datagrams from that same socket, supplies monotonic
milliseconds and calls poll while idle. There is no socket allocation or
hidden callback/event loop in the core.

The new sender intentionally permits only one packet outstanding, uses a
fixed 1,200-byte outbound IPv4 MTU (1,152 payload bytes), and backs off its
initial 1-second retransmission timeout to 30 seconds. Six retries without
ACK close it. This caps traffic under loss, without copying BBR or claiming
stock throughput/congestion behavior. There is no RTT estimator, MTU search,
stream migration, unordered-message API or data relay. Only positive windows
of at least one maximum outbound payload admit ordinary new data; zero-window
probing of the single queued packet is bounded by the timeout. A positive
sub-packet peer window waits for a newer window update. Existing retransmits
retain their one-packet ownership, even if the window subsequently shrinks.
This is a restricted direct-stream implementation, not full UDX conformance.

Data remains in the TX buffer until cumulative acknowledgment. SACK blocks
are bounds-checked but do not free this single outstanding packet, so a lost
cumulative ACK is safe to retransmit. The RX pool retains ordered or reordered
bytes before acknowledgment; malformed/foreign source, version, ID, lengths,
flag combinations and impossible ACKs do not mutate stream state. Old ACKs
do not discard new receive data. Incoming MTU-probe padding is stripped with
checked offsets. Source checks do not replace record authentication.
Advertised peer windows follow wrap-aware sequence/ACK ordering, matching
libudx's send_wl1/send_wl2 rule; an older packet cannot replace fresh credit
with a stale zero or positive sub-packet value. A regression reproduces the
stalling stale-window overwrite before the correction.

The application drains the RX pool into its bounded record parser, releasing
raw slots incrementally and calling `hdt_transport_consumed` to schedule a
window update. A slow application retains its ownership/backpressure; it
must not ACK discarded data or release a partially consumed slot. Pure ACKs
report cumulative retention and fixed-slot credit. No SACK blocks are emitted,
so inbound reordering/loss can require the peer's RTO rather than fast recovery.

Standalone END is queued only after pending TX is acknowledged. Incoming END
may be standalone or combined with DATA; its ordered EOF waits for retained
payloads to be consumed. Out-of-order END waits for the gap, and a premature
END before already retained later DATA is rejected. DESTROY and retry
exhaustion are terminal; callers must separately wipe Noise/record secrets
and finish their socket lifecycle. This module stores ciphertext, not keys.

`transport_boundaries` covers foreign source/ID, invalid magic/version/offset,
flags, malformed SACK, impossible ACKs, old ACK with valid data, combined and
out-of-order END, EOF across wrap, retry exhaustion, zero-window probing and
DESTROY. `transport_stock_interop` uses original unmodified libudx on one
real ephemeral loopback UDP socket and this transport on the other. It
exchanges 3,000 bytes toward tiny and 1,044 toward stock, drops the first tiny
DATA, duplicates retransmitted DATA, starts both directions near sequence
wrap, and verifies ordered bidirectional EOF. The tiny clock is advanced
deterministically; this is not hardware timing evidence. The existing stock
gap retransmission test independently covers the RX credit trap.

Authenticated Noise and record compatibility have separate reference tests;
joining discovery, handshake RPC and this stream into one full connection is
still pending at this checkpoint. No full endpoint, RA4M1 driver, real-board
runtime or production congestion/performance claim follows from these tests.
