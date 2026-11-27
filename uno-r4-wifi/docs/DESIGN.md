# Tiny endpoint design

Status: resumed, 2026-11-22. The direct connector completes encrypted host
exchange with a real native peer. Listener, NAT support and complete RA4M1
board runtime/fit remain pending; see CONNECTOR.md and R4-MEASUREMENTS.md.

## Role selection

Expose separate connector and listener function sets over the shared core.
An application can use either role; enabling both is a separate resource profile
whose memory fit must be measured. NAT traversal remains independently optional.
These are application roles, not full DHT routing-node roles.

| Role | Additional responsibilities |
|---|---|
| Connector | Find a selected peer, initiate its handshake, establish the stream |
| Listener | Announce/refresh its own presence through external DHT nodes, receive rendezvous handshakes, accept an authenticated stream, withdraw its announcement on shutdown |
| Shared core | UDP/RPC framing, crypto, bounded stream transport, record processing, timers and lifecycle |

The listener does not store other nodes' DHT data. Its own announcement is a
publication to external nodes, not a local storage service or routing table.
Its announcement, refresh, handshake and shutdown state needs a separate bound.

Keep role-specific implementation in separate compilation units/archive members
without global initialization or registration that drags both roles into every
image. Verify connector-only and listener-only link maps and RAM separately;
different function names alone do not establish size savings. Both roles reuse
the same crypto/transport routines rather than duplicating them.

## Initial constrained profiles

- IPv4, one connection attempt and one established encrypted stream at a time
  per supported single-role profile. No unbounded listener handshake queue;
  handling excess attempts must be specified without an identity allowlist.
- No key allowlist, authorization database or application firewall callback.
  The connector permits any selected target key; the listener permits any
  correctly authenticated remote key. Exact Noise identity proof, connector
  expected-remote-key checking, packet validation and authenticated encryption
  remain required.
- No DHT data storage service, persistent routing table, k-buckets or routing-
  node service. Only the listener includes its own announcement lifecycle.
- Discovery by public key using bootstrap addresses and a bounded temporary
  lookup workspace. Capping a walk can miss a reachable peer; return an
  explicit unsuccessful/incomplete result rather than claim absence globally.
- Direct connection to a reachable peer first. Rendezvous handshake forwarding
  is retained; blind application-data relay and relay upgrades are omitted.
- Proposed maximum application record: 1,024 plaintext bytes in either
  direction. This requires a cooperative remote application and is not an
  existing negotiated HyperDHT capability. It does not promise arbitrary
  application/Holesail compatibility. Detect an oversized declared encrypted
  frame before allocating its body and terminate cleanly.

The proposed limit applies to each encrypted write/record, not just each logical
application message. With the existing framing, 1,024 plaintext bytes become
1,041 ciphertext bytes plus a 3-byte length prefix: 1,044 stream bytes. The
initial 56-byte header frame has separate validation rules. Neither coalescing
small application messages into one large write nor splitting a record into
small UDP packets makes an oversized record acceptable.

Role choice is independent of NAT status. A listener without traversal support
needs an already reachable endpoint; advertising a key does not make a NAT
mapping reachable. Resource limits remain in force even with no key allowlist.

## Components

1. **Discovery:** compact RPC codecs, transaction/source matching, retry/
   deadline handling and a fixed candidate/request workspace. Keep no lookup
   state after it is no longer needed. Start with one outstanding request;
   determine sufficient response parsing and candidate limits by source review.
2. **Authenticated connection:** exact HyperDHT prologue and
   Noise_IK_Ed25519_ChaChaPoly_BLAKE2b handshake, payload/version handling,
   rendezvous control RPC and UDX stream negotiation. Do not substitute a
   different Noise identity scheme.
3. **Bounded stream transport:** preserve required UDX packet IDs, sequence/
   ACK/SACK handling, ordering, retransmission, real receive-credit accounting,
   congestion behavior, timers and end/destroy semantics. Limit local queues;
   do not replace reliability with unreliable datagrams while claiming ordinary
   HyperDHT stream compatibility.
4. **Encrypted records:** exact SecretStream header/framing/authentication,
   bounded frame and write queues, no unauthenticated partial plaintext exposed
   to the application. Preserve a retransmission payload until it is safe to
   release. Neither one connection nor small UDP packets bounds record memory.
5. **Platform adapter:** UDP send/receive, monotonic time, cryptographically
   secure randomness and identity persistence supplied by the board/platform.
   A cooperative poll loop avoids requiring libuv or a dedicated RTOS task.

These are logical boundaries; do not introduce five separately installed
packages. Start with one core library exposing independently selectable
connector/listener APIs, plus one optional traversal library.

## Optional NAT/punch library

The core can omit this library entirely. Its later responsibilities would be
address observation, authenticated punch coordination and supported punching
strategies, preserving the same UDP socket and mapping through establishment.
No classifier is required in the first core profile. An optional implementation
may use a restricted strategy instead of a full classifier, with explicit
failure on unsupported network conditions.

Being behind NAT does not determine mapping/filtering behavior. No traversal
means no guarantee of reaching peers behind NAT. Removing blind data relay
also removes its fallback when a direct path cannot be established. Traversal
memory must be budgeted independently before adding it to the RA4M1 target.

## Initial RAM ceilings

These are admission criteria for feasibility, not estimates of existing code:

| Category | Proposed ceiling |
|---|---:|
| Core static state, crypto state and fixed packet/record pools | 10 KiB |
| Worst-case active stack, including crypto calls and interrupts | 8 KiB |
| Board networking/driver/platform data outside the core | 6 KiB |
| Application data reserve | 6 KiB |
| Unallocated safety margin | 2 KiB |
| Total physical SRAM budget | 32 KiB |

If a category cannot meet its ceiling, revise the design and demonstrate a
safe total budget before continuing; do not silently consume the application
reserve. Include all overlapping phases, buffers and stack peaks. A networking
coprocessor's memory is outside RA4M1 SRAM and must be reported separately.
For Uno R4 Minima, a networking interface is an additional requirement.

The whole target has 256 KiB flash. As an initial goal, a minimum connected
application image should fit within 224 KiB, leaving 32 KiB for growth where
the board linker/bootloader layout permits. Measure the actual usable region,
crypto/platform linked cost and application cost before claiming this target.

## Receive credit is a separate design gate

The current libudx sender converts advertised credit into a packet count using
floor(credit / maximum payload). Its IPv4 payload is MTU minus 48 bytes: 1,152
at the base 1,200-byte MTU, and 1,452 at MTU 1,500. Merely advertising 1,044
bytes for one complete small record can therefore stall ordinary transmission.
Our own transmit MTU does not constrain the remote sender's MTU.

A stock peer also starts with 4 MiB send credit before observing the remote
advertisement. That is not a 4 MiB allocation, but initial bursts and packets
already in flight must be handled by bounded admission. Credit cannot replace
fixed packet-pool accounting. Do not ACK reliable bytes that were discarded.

Before implementation, make a byte-counted state/ownership table for one legal
record: ciphertext/plaintext overlap, retransmission lifetime, receive reorder
slots and metadata, header/body crossing packet boundaries, advertised credit
and every credit update. Trace progress with peer MTU 1,200 and 1,500, normal
credit, zero credit and positive sub-packet residual credit. Include initial
bursts, oversized records and probes. Establish both forward progress and
bounded storage. The focused four-slot/one-free-slot stock-sender gate now
passes; see UDX-RX.md and TRANSPORT.md for coverage and restricted scope.

## Feasibility gates and reuse

1. Inventory reusable code and test vectors from hyperdht-cpp and libudx;
   record exact revisions, file-level licenses and required dependencies.
   Include the embedded RNG/TLS correction. Reuse authenticated crypto routines
   and test vectors rather than inventing cryptographic replacements.
2. Define the narrow peer contract, state transitions, ownership and buffer
   lifetimes. Review the 1 KiB record limit and constrained receive-credit
   behavior against an ordinary peer implementation. Reject incompatible
   assumptions before writing the transport.
3. Implementation is now authorized. Add a pinned Nix build and
   native tests first. Prove bounded parser behavior, lookup termination,
   authentication failures and teardown. Then cross-link RA4M1 and inspect
   map/stack evidence against this budget before adding NAT support.
4. Interoperate with an existing HyperDHT peer under loss, duplication,
   reordering, backpressure, sequence wrap and disconnect. Check small and
   oversized encrypted records, invalid packets/tags and bounded resource use.
5. Validate connector-only and listener-only profiles independently on the
   real board and measure peak memory/stack and flash use. Listener checks also
   cover discovery by an existing client, announcement refresh/withdrawal,
   sequential connections from different identities and bounded handshake load.
   Keep optional NAT feasibility and testing separate from the core result.

The original design-only stage is complete. Implementation work takes place only
in this new repository or separate reviewer checkouts; existing repositories
remain read-only. Axiometa Arduino packaging is a separate project.

## Current evidence

Read-only source review of hyperdht-cpp and libudx establishes that the current
connect path uses discovery, rendezvous Noise handshake, UDX and SecretStream.
The current SecretStream permits a 24-bit encrypted frame length and accumulates
complete records. Current libudx includes dynamically growing packet queues;
its advertised receive credit is not an allocated-memory measurement.

Neither these sources nor previous ESP32 successes establish RA4M1 fit.
Actual component+crypto link sizes are now measured in R4-MEASUREMENTS.md.
Full board support and cumulative runtime stack peaks remain unverified.
