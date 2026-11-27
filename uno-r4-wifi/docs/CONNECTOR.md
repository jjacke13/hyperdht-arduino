# Direct connector checkpoint — 2026-11-22

The reusable C99 `hdt_connector` now joins a selected rendezvous address,
PEER_HANDSHAKE RPC, authenticated Noise, the bounded UDX transport and encrypted
records. The existing bounded discovery workspace can supply the rendezvous;
an application may also provide a known direct listener endpoint. It still
owns the UDP socket, clock and entropy. The core performs no I/O or allocation.

This is an initial **direct, reachable IPv4 connector profile**. It advertises
OPEN and accepts only an authenticated OPEN responder with UDX/SecretStream v1.
Use it only when that reachability assumption is true. Neither endpoint's NAT
behavior is inferred from outgoing/incoming role. NAT punching, data relays,
source migration, announcement/listener service and arbitrary application-write
sizes remain outside this checkpoint. It permits any selected key while
requiring the exact selected peer's identity proof.

## Calling contract

1. Initialize libsodium with an appropriate platform random source. Run bounded
   discovery on a bound UDP socket, or obtain a rendezvous endpoint explicitly.
2. Begin with the selected key, persistent identity seed, a fresh CSPRNG
   ephemeral seed, random stream ID and transaction ID. Do not reinitialize a
   live context. Inputs, output scratch and state must not overlap.
3. Poll with monotonic milliseconds and caller-owned datagram scratch. Send the
   returned bytes to the returned destination using the same socket. The RPC
   request is retained unchanged for three attempts at 1-second intervals.
   Only actual-source + response-type + transaction-ID matches advance it.
4. Feed received datagrams with their actual source. Once Noise authenticates,
   validate the peer's bounded metadata and adopt its negotiated stream ID and
   sequence. The code copies final keys into a small stack temporary, wipes the
   handshake union, creates the record state, wipes the temporary and queues
   the SecretStream header as the first reliable stream write.
5. READY requires the remote encrypted header plus cumulative ACK of the local
   header. Then write at most 1,024 plaintext bytes when no TX is outstanding.
   The record is sealed directly into the retained transport buffer. A zero
   write result means backpressure/invalid size/closed state; the seal is not
   attempted while the buffer is busy.
6. Read returns only authenticated plaintext, borrowed until release or error/
   clear. Release drains retained ciphertext incrementally. A partial packet
   cursor preserves coalesced records without discarding a packet remainder.
   The receiver can hold a pending plaintext record while UDX ACK/control
   processing continues. Poll remains necessary for idle window updates.
7. End queues reliable EOF once pending TX is acknowledged. Remote EOF at a
   partial encrypted frame is fatal. Remote EOF half-closes only reception;
   local writes remain allowed until local end, matching a normal duplex stream.
   A regression authenticates record headers, receives FIN, then queues a reply.
   Clear wipes the entire context and callers
   finish their socket lifecycle. Fatal errors abort locally; this first adapter
   does not promise delivery of a remote DESTROY on an authentication failure.

There are no application callbacks or concurrent calls inside the core. A failed
context cannot be used again until explicit begin. Authentication/protocol errors
clear keys and all buffered plaintext. The final socket/driver shutdown belongs
to the caller, so no success is inferred from an API timeout alone.

## Memory ownership

On the tested x86_64 ABI, `sizeof(hdt_connector)` is **9,272 bytes**. Its union
overlays a 552-byte Noise state plus retained handshake request with the larger
connected transport+record state. Discovery (280 bytes on the recorded ARM ABI)
is a separate temporary workspace and may be discarded after begin. Caller
datagram scratch, platform buffers, identity storage, libsodium globals and call
stack are additional. The 9,272-byte host number is not a target peak-RAM claim.

## Evidence

`connector_reference_session` binds two ephemeral loopback UDP sockets. The
reference fixture uses the original request/response and peer-record codecs,
handshake payload/wrapper codecs, NoiseIK, SecretStream and unmodified libudx.
It is a local fixture assembled from original protocol implementations, **not
a full production HyperDHT routing node**.

The test performs FIND_PEER, PEER_HANDSHAKE Noise authentication, reliable
encrypted header exchange, two 1KiB application records in each direction,
held-plaintext backpressure with coalesced records, and bidirectional EOF.
It drops the first discovery request, first handshake request and first tiny
stream DATA, and duplicates subsequent tiny DATA. A controlled clock advance
triggers the tiny retries without waiting a real second; the original libuv
side runs its normal loop. Separate transport tests cover sequence wrap and
combined DATA/END behind unread data in both arrival orders, including wrap.

All 32 supported Noise-payload optional-flag combinations are decoded from
original encoder output; selected minimal payloads match the original bytes.
Boundary tests check truncation, wrapper modes, transaction/source rejection,
handshake capacity and timeout limits. Original Noise tests independently
check mutations of every byte in both messages and wrong identities.

### Real native-node check

With `HDT_FULL_NODE_REFERENCE=ON`, a separate test builds the read-only original
hyperdht-cpp project using its normal CMake library target, native Server and
SecretStreamDuplex. Its own normal build-time UDX hardening is preserved. Tiny
uses an explicitly supplied listener address and selected key: this test
authenticates, sends a 1KiB message, receives the encrypted echo, and completes
graceful bidirectional EOF against that actual server. No peer protocol handlers
are replaced by a fixture in this test. The original node's observed address is
configured to its real loopback endpoint; it needs no NAT traversal.

The child runs in fresh Linux user/network namespaces with only loopback up,
so even the reference announcer's default bootstrap attempts cannot reach public
or lab nodes. This requires `unshare` and `ip`. Both sanitizer and Release runs
pass, bringing the full optional host suite to eleven checks. The original
project's normal nonfatal warning policy is retained for reference code; the
tiny core and tests still compile with -Werror. Original source is unchanged;
all generated files and libraries go into the tiny build directory.

Remaining gates: discovery/relay paths across ordinary nodes beyond the local
fixture, whole call-stack peaks, board networking/entropy, listener-only and
combined-role profiles, NAT/punching and real hardware timing. Linked target
component evidence is in R4-MEASUREMENTS.md. Host sanitizers do not close these
board/runtime gates.
