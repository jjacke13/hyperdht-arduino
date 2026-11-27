# Reused code

The opt-in `platform/arduino-r4/entropy.c` is new integration glue for the
existing platform hook. Its SCE5 API signatures and initialization contract were
checked against ArduinoCore-renesas 1.6.0
`424e86eff92d37f72123c2b641dd8bbf06a38b47` and its FSP submodule
`9b5a853bd1b0040e7bb4350b7cf36c7767c9b4f8`. No Arduino/Renesas implementation
or SDK header is copied into the library. Vendor dependencies keep their own
licenses; the host fake header is a test-only contract fixture. See
[entropy scope and source links](docs/R4-ENTROPY.md).

The first fixed-buffer C components adapt selected algorithms and wire layouts
from hyperdht-cpp at `66f75c97c9bdb1c77dafd761f18043063d1c064f` (LGPL-3.0;
original license retained in LICENSE). Existing source checkouts are read-only.

| Local component | Source parts adapted |
|---|---|
| src/rpc.c | src/compact.cpp: checked cursors and compact uint/address/buffer encoding; src/messages.cpp: request/response envelope order and flags; src/dht_messages.cpp: peer record layout |
| src/discovery.c | src/dht_ops.cpp: BLAKE2b public-key target and FIND_PEER command; src/query.cpp: transient candidate walk/retry concept; src/rpc.cpp: transaction/source association |
| src/record.c | src/secret_stream.cpp: unchanged namespace constants, BLAKE2b stream IDs keyed by the full 64-byte Noise handshake hash, header/framing, libsodium push/pull and empty-keepalive suppression |
| src/noise.c | src/noise_wrap.cpp: IK token order, HMAC/HKDF glue, Ed25519 DH scalar preparation, nonce layout, symmetric transcript and split; src/peer_connect.cpp: fixed PEER_HANDSHAKE prologue |
| src/handshake.c | src/peer_connect.cpp: noisePayload and handshake outer-wrapper codecs, bounded optional-field traversal |
| src/connector.c | src/connect.cpp and src/peer_connect.cpp: selected-peer handshake request, response correlation, authenticated payload checks and transport adoption |

The handshake/connector additions use the same clean `53c9bb52` source revision
as Noise. `src/compact_internal.h` moves the existing checked compact cursor
helpers out of rpc.c so new codecs reuse them without allocating or duplicating
the decoder. The connector overlays handshake and connected state and keeps
no copied identity secret after adoption. Host test code adapts stock libudx
and original peer codecs into a local fixture. The optional full-node test also
builds the original native node/server in the tiny build directory and runs it
only inside a loopback-only network namespace.

The Noise adaptation was read and tested against clean source files at
`53c9bb52fe9eea6dedea2ad87b29fcabb576a270` on 2026-11-22. It uses the same
libsodium primitives, fixed caller-owned output buffers and a 512-byte payload
ceiling. Fresh ephemeral seeds must come from platform CSPRNG per attempt;
deterministic seeds in tests are not a production entropy source. Both roles
are compared byte-for-byte with original NoiseIK and subsequently exchange
encrypted records with original SecretStream. This proves component transcript
compatibility, not a complete DHT connection or board entropy/runtime support.

The separately licensed `src/udx_rx.c` adaptation is Apache-2.0, from libudx
`759bf7641347f65a1896d4bc4c3b1606a459f466`. Its original NOTICE and license are
under `licenses/`; detailed provenance and remaining transport layers are in
[docs/UDX-RX.md](docs/UDX-RX.md). Root LICENSE does not replace those notices.
`src/transport.c` is also Apache-2.0 and adapts that same libudx revision's
20-byte wire header, sequence/ACK and lifecycle concepts to a fixed-buffer,
one-outstanding-packet sender. It does not copy BBR/window-filter code. See
[docs/TRANSPORT.md](docs/TRANSPORT.md) for its deliberately restricted profile.

Allocation-free views, fixed queues, caps and the cooperative poll adapter are
new adaptation glue. The discovery walk is a bounded FIFO exploration, not a
complete Kademlia routing implementation. Strict envelope/peer trailing-byte
rejection and the four-relay result cap are explicit restricted-profile limits.
Discovery responses are hints, not authenticated identities or proof of absence.

Host hashing uses the original libsodium crypto_generichash API. The ARM
cross-link now includes actual target libsodium; its configuration, linked
size and remaining board-runtime limits are recorded in docs/R4-MEASUREMENTS.md.
The record layer requires completed, authenticated Noise keys; it does not
perform or replace the handshake. It keeps ciphertext until authentication,
wipes all state on fatal failure, and never exposes partial plaintext. Its
1 KiB plaintext-per-write cap is a cooperative application restriction.

Reference tests compile original codecs and SecretStream directly from a caller-supplied
read-only checkout; that checkout is not part of the installed library.

`src/publish.c` adapts the signature preimage and namespace functions in
`src/announce_sig.cpp` and `src/dht_messages.cpp`, along with the
FIND_PEER token exchange and ANNOUNCE/UNANNOUNCE ordering in
`src/announcer.cpp`, from hyperdht-cpp
`35514620b5e050817fe15255333f6ab4427f469c` (LGPL-3.0).
It uses one caller-selected external node and one in-flight exchange.
The caller keeps the signing key and schedules signed re-announcements.

`src/listener.c` promotes the host fixture's reachable responder into a
shared C module. It adapts the Noise-to-stream transition and direct/relayed
handshake reply envelope from `src/server_connection.cpp` and `src/router.cpp`
at the same hyperdht-cpp revision. One cached reply associates an exact Noise
message with a single client endpoint. Only selected rendezvous nodes may
forward a client address.

`src/publish_walk.c` adapts the iterative FIND_PEER walk and XOR result
selection in `src/query.cpp` and `src/announcer.cpp` at the same
hyperdht-cpp revision. It follows dht-rpc's six-byte IPv4 address encoding
for candidate node IDs and ranks successful replies by their returned ID.
The initial walk tracks up to eight candidates and 24 visits and retains
three token-bearing endpoints. The listener does not retain a routing table.

`src/listener_punch.c` adapts the responder rounds, address-token echo
and server reply framing from `src/server.cpp`, `src/holepunch.cpp`
and `src/router.cpp` at the same hyperdht-cpp revision. Listener mapping
classification uses corroborating publication ACKs on the same socket.
It schedules bounded direct probes after an authenticated, token-matched
commit. Only the stable-mapping and open-client paths are implemented.

The listener's LAN shortcut follows the same hyperdht-cpp server and
connector paths at that revision. The Uno offers its private address in
the authenticated Noise reply after matching the peer's external IPv4
and private subnet. It answers the stock LAN ping and keeps UDX bound to
the endpoint advertised by the authenticated peer.
