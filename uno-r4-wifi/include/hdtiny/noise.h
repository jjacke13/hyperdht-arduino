/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_NOISE_H
#define HDTINY_NOISE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { HDT_NOISE_PAYLOAD_MAX = 512, HDT_NOISE_MESSAGE_MAX = 608 };
typedef struct {
    uint8_t local[32], secret[64], ephemeral[32], ephemeral_secret[64];
    uint8_t remote[32], remote_ephemeral[32], digest[64], chain[64], key[32];
    uint8_t tx[32], rx[32], hash[64];
    uint32_t nonce;
    uint8_t initiator, phase; /* 0: msg1; 1: msg2; 2: complete; 255: failed/clear */
} hdt_noise;
/* Exact HyperDHT Noise IK, with its fixed PEER_HANDSHAKE prologue.
 * sodium_init/platform randomness must be installed first. Both seeds are
 * 32 bytes; ephemeral_seed MUST be fresh unpredictable CSPRNG output per
 * attempt. remote_key is mandatory for connector, NULL for listener.
 * Init only fresh/cleared objects. No overlapping inputs/state/output buffers.
 * One serialized caller; no callbacks, heap allocation or retained input views. */
int hdt_noise_init(hdt_noise *n, int initiator, const uint8_t static_seed[32],
                   const uint8_t ephemeral_seed[32], const uint8_t remote_key[32]);
/* Capacity/order/argument failures leave state untouched. Invalid/authentication
 * failures during receive wipe state and the candidate plaintext output.
 * Empty payloads are supported; output length is set to zero on failure. */
int hdt_noise_send(hdt_noise *n, const uint8_t *payload, size_t size,
                   uint8_t *out, size_t capacity, size_t *written);
int hdt_noise_receive(hdt_noise *n, const uint8_t *message, size_t size,
                      uint8_t *payload, size_t capacity, size_t *written);
/* Read tx/rx/hash/remote only once complete. Completion wipes handshake secrets. */
int hdt_noise_complete(const hdt_noise *n);
void hdt_noise_clear(hdt_noise *n);
#ifdef __cplusplus
}
#endif
#endif
