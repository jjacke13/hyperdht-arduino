/* SPDX-License-Identifier: LGPL-3.0-only
 * Fixed-buffer adaptation of hyperdht-cpp/src/noise_wrap.cpp and the
 * PEER_HANDSHAKE prologue from src/peer_connect.cpp; see PROVENANCE.md.
 * Crypto primitives remain libsodium; this adapts framing/state ownership.
 */
#include "hdtiny/noise.h"
#include <sodium.h>
#include <string.h>

static void mix_hash(hdt_noise *n, const uint8_t *p, size_t size) {
    crypto_generichash_state state;
    crypto_generichash_init(&state, NULL, 0, 64);
    crypto_generichash_update(&state, n->digest, 64);
    if (size) crypto_generichash_update(&state, p, size);
    crypto_generichash_final(&state, n->digest, 64);
    sodium_memzero(&state, sizeof(state));
}

/* Original HMAC-BLAKE2b: unkeyed hash of 128-byte pads, not keyed BLAKE2b. */
static void hmac(uint8_t out[64], const uint8_t key[64],
                  const uint8_t *a, size_t size, uint8_t suffix, int append) {
    uint8_t pad[128] = {0}, inner[64];
    crypto_generichash_state state;
    size_t i;
    memcpy(pad, key, 64);
    for (i = 0; i < sizeof(pad); ++i) pad[i] ^= 0x36;
    crypto_generichash_init(&state, NULL, 0, 64);
    crypto_generichash_update(&state, pad, sizeof(pad));
    if (size) crypto_generichash_update(&state, a, size);
    if (append) crypto_generichash_update(&state, &suffix, 1);
    crypto_generichash_final(&state, inner, 64);
    for (i = 0; i < sizeof(pad); ++i) pad[i] ^= 0x36 ^ 0x5c;
    crypto_generichash_init(&state, NULL, 0, 64);
    crypto_generichash_update(&state, pad, sizeof(pad));
    crypto_generichash_update(&state, inner, sizeof(inner));
    crypto_generichash_final(&state, out, 64);
    sodium_memzero(&state, sizeof(state));
    sodium_memzero(pad, sizeof(pad));
    sodium_memzero(inner, sizeof(inner));
}

static void hkdf(const uint8_t chain[64], const uint8_t *dh, size_t size,
                   uint8_t first[64], uint8_t second[64]) {
    uint8_t prk[64];
    hmac(prk, chain, dh, size, 0, 0);
    hmac(first, prk, NULL, 0, 1, 1);
    hmac(second, prk, first, 64, 2, 1);
    sodium_memzero(prk, sizeof(prk));
}

static int mix_key(hdt_noise *n, const uint8_t secret[64], const uint8_t peer[32]) {
    uint8_t scalar[64], shared[32], ck[64], key[64];
    int ok;
    crypto_hash_sha512(scalar, secret, 32);
    scalar[0] &= 248; scalar[31] &= 127; scalar[31] |= 64;
    ok = crypto_scalarmult_ed25519_noclamp(shared, scalar, peer) == 0;
    if (ok && !sodium_is_zero(shared, sizeof(shared))) {
        hkdf(n->chain, shared, sizeof(shared), ck, key);
        memcpy(n->chain, ck, 64); memcpy(n->key, key, 32);
        n->nonce = 0;
    } else ok = 0;
    sodium_memzero(scalar, sizeof(scalar)); sodium_memzero(shared, sizeof(shared));
    sodium_memzero(ck, sizeof(ck)); sodium_memzero(key, sizeof(key));
    return ok;
}

static int cipher(hdt_noise *n, int encrypt, const uint8_t *in, size_t size,
                    uint8_t *out) {
    uint8_t nonce[12] = {0};
    unsigned long long length = 0;
    int rc;
    if (n->nonce == UINT32_MAX) return 0;
    nonce[4] = (uint8_t)n->nonce; nonce[5] = (uint8_t)(n->nonce >> 8);
    nonce[6] = (uint8_t)(n->nonce >> 16); nonce[7] = (uint8_t)(n->nonce >> 24);
    if (encrypt)
        rc = crypto_aead_chacha20poly1305_ietf_encrypt(out, &length, in, size,
                 n->digest, 64, NULL, nonce, n->key);
    else
        rc = crypto_aead_chacha20poly1305_ietf_decrypt(out, &length, NULL, in, size,
                 n->digest, 64, nonce, n->key);
    if (rc) return 0;
    ++n->nonce;
    mix_hash(n, encrypt ? out : in, encrypt ? size + 16 : size);
    return 1;
}

void hdt_noise_clear(hdt_noise *n) {
    if (n) { sodium_memzero(n, sizeof(*n)); n->phase = 255; }
}

static int fail(hdt_noise *n) { hdt_noise_clear(n); return 0; }

static void finish(hdt_noise *n) {
    uint8_t first[64], second[64];
    hkdf(n->chain, NULL, 0, first, second);
    memcpy(n->tx, n->initiator ? first : second, 32);
    memcpy(n->rx, n->initiator ? second : first, 32);
    memcpy(n->hash, n->digest, 64);
    sodium_memzero(first, sizeof(first)); sodium_memzero(second, sizeof(second));
    sodium_memzero(n->secret, sizeof(n->secret));
    sodium_memzero(n->ephemeral_secret, sizeof(n->ephemeral_secret));
    sodium_memzero(n->chain, sizeof(n->chain)); sodium_memzero(n->key, sizeof(n->key));
}

int hdt_noise_init(hdt_noise *n, int initiator, const uint8_t seed[32],
                   const uint8_t ephemeral_seed[32], const uint8_t remote[32]) {
    static const char protocol[] = "Noise_IK_Ed25519_ChaChaPoly_BLAKE2b";
    static const char name[] = "hyperswarm/dht";
    uint8_t input[33] = {0}, prologue[32];
    if (!n || !seed || !ephemeral_seed || (initiator && !remote)) return 0;
    memset(n, 0, sizeof(*n)); n->initiator = !!initiator;
    crypto_sign_seed_keypair(n->local, n->secret, seed);
    crypto_sign_seed_keypair(n->ephemeral, n->ephemeral_secret, ephemeral_seed);
    memcpy(n->digest, protocol, sizeof(protocol) - 1);
    memcpy(n->chain, n->digest, 64);
    crypto_generichash(input, 32, (const uint8_t *)name, sizeof(name) - 1, NULL, 0);
    crypto_generichash(prologue, 32, input, sizeof(input), NULL, 0);
    mix_hash(n, prologue, sizeof(prologue));
    if (initiator) memcpy(n->remote, remote, 32);
    mix_hash(n, initiator ? remote : n->local, 32);
    return 1;
}

int hdt_noise_send(hdt_noise *n, const uint8_t *payload, size_t size,
                   uint8_t *out, size_t capacity, size_t *written) {
    size_t offset;
    if (written) *written = 0;
    if (!n || !written || !out || (!payload && size) || size > HDT_NOISE_PAYLOAD_MAX ||
        n->phase > 1 || n->phase != (n->initiator ? 0 : 1)) return 0;
    offset = n->initiator ? 80 : 32;
    if (capacity < offset + size + 16) return 0;
    memcpy(out, n->ephemeral, 32); mix_hash(n, n->ephemeral, 32);
    if (n->initiator) {
        if (!mix_key(n, n->ephemeral_secret, n->remote) ||
            !cipher(n, 1, n->local, 32, out + 32) ||
            !mix_key(n, n->secret, n->remote)) return fail(n);
    } else if (!mix_key(n, n->ephemeral_secret, n->remote_ephemeral) ||
               !mix_key(n, n->ephemeral_secret, n->remote)) return fail(n);
    if (!cipher(n, 1, payload, size, out + offset)) return fail(n);
    ++n->phase;
    if (n->phase == 2) finish(n);
    *written = offset + size + 16;
    return 1;
}

int hdt_noise_receive(hdt_noise *n, const uint8_t *message, size_t size,
                      uint8_t *payload, size_t capacity, size_t *written) {
    size_t offset, plain_size;
    if (written) *written = 0;
    if (!n || !written || !message || !payload || n->phase > 1 ||
        n->phase != (n->initiator ? 1 : 0)) return 0;
    offset = n->initiator ? 32 : 80;
    if (size < offset + 16 || size > offset + 16 + HDT_NOISE_PAYLOAD_MAX)
        return fail(n);
    plain_size = size - offset - 16;
    if (capacity < plain_size) return 0;
    memcpy(n->remote_ephemeral, message, 32); mix_hash(n, message, 32);
    if (!n->initiator) {
        if (!mix_key(n, n->secret, n->remote_ephemeral) ||
            !cipher(n, 0, message + 32, 48, n->remote) ||
            !mix_key(n, n->secret, n->remote)) goto bad;
    } else if (!mix_key(n, n->ephemeral_secret, n->remote_ephemeral) ||
               !mix_key(n, n->secret, n->remote_ephemeral)) goto bad;
    if (!cipher(n, 0, message + offset, size - offset, payload)) goto bad;
    ++n->phase;
    if (n->phase == 2) finish(n);
    *written = plain_size;
    return 1;
bad:
    sodium_memzero(payload, plain_size);
    return fail(n);
}

int hdt_noise_complete(const hdt_noise *n) { return n && n->phase == 2; }
