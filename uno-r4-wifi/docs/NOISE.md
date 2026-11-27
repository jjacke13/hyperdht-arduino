# Bounded Noise IK handshake

`hdt_noise` adapts the existing HyperDHT Noise IK state machine to caller-owned
C99 buffers, without heap allocation. It preserves the exact
Noise_IK_Ed25519_ChaChaPoly_BLAKE2b protocol, fixed PEER_HANDSHAKE prologue,
Ed25519 scalar preparation, HMAC-BLAKE2b HKDF, nonce layout, 64-byte transcript
hash and role-dependent transport split. All crypto primitives are libsodium.
See PROVENANCE.md for source revisions and licenses.

The connector supplies the exact expected listener public key. The listener
learns the authenticated connector key; there is no identity allowlist. Each
attempt accepts at most 512 bytes of encrypted-handshake plaintext, so the
first message is at most 608 bytes and the second at most 560. This limit
must also apply to future handshake payload codecs. It is a local resource
policy, not a capability negotiated with an ordinary peer.

The platform supplies a persistent identity seed and a fresh unpredictable
ephemeral seed for each attempt, using a cryptographically secure generator.
The test seeds are deliberately deterministic and must not be copied into
firmware. Init is for fresh/cleared state only; a live state must not be
reinitialized. Serialize callers and keep all input/output/state buffers
nonoverlapping. The implementation retains no caller pointers.

Successful completion exposes tx/rx keys, remote identity and the complete
64-byte handshake hash, and wipes static/ephemeral secret copies, cipher key
and chaining key. The record layer still requires platform randomness to
generate its SecretStream header. Call clear after adopting the results or
aborting. A malformed or authentication-failed receive poisons/wipes the state;
candidate plaintext is wiped after cryptographic failure. Argument/order and
insufficient-capacity errors do not consume the state and may be retried.

`noise_reference_parity` compares both roles against unmodified original
NoiseIK with fixed ephemerals. It checks exact message bytes, both transport
keys, remote identity, all 64 hash bytes, payload sizes 0/1/512, truncations,
low-order/incorrect identities, wrong ordering, undersized output buffers and
one-bit mutations at every byte of both messages. It then uses those keys to
exchange 1KiB application records with original SecretStream in both roles.
All six host suites pass under ASan/UBSan and Release at this checkpoint.

This is component interoperability. RPC request correlation, payload negotiation,
socket lifecycle, UDX sender/receiver integration and a complete discovered
connection remain separate gates. No hardware, board entropy, cycle timing,
whole-stack peak or full firmware fit follows from these tests.
