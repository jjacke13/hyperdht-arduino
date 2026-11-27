/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_RECORD_H
#define HDTINY_RECORD_H
#include <stddef.h>
#include <stdint.h>
#include <sodium.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { HDT_RECORD_PLAIN_MAX = 1024, HDT_RECORD_CIPHER_MAX = 1041,
       HDT_RECORD_HEADER_SIZE = 59, HDT_RECORD_WIRE_MAX = 1044 };
/* One serialized caller; no reentrancy or live-context reinitialization. */
typedef struct {
    crypto_secretstream_xchacha20poly1305_state tx, rx;
    uint8_t expected_id[32], rx_key[32];
    uint8_t cipher[HDT_RECORD_CIPHER_MAX], plain[HDT_RECORD_PLAIN_MAX], prefix[3];
    size_t body_size, body_used, plain_size;
    uint8_t prefix_used, header_received, pending, active, failed;
} hdt_record;
typedef enum { HDT_RECORD_ERROR = -1, HDT_RECORD_INPUT = 0, HDT_RECORD_MESSAGE = 1 } hdt_record_result;
/* Keys/hash are outputs of the exact authenticated HyperDHT Noise handshake.
 * Call sodium_init and install platform randomness first. No overlapping buffers.
 * The caller queues the returned header before any sealed data on the stream. */
int hdt_record_init(hdt_record *r, const uint8_t tx_key[32], const uint8_t rx_key[32],
                    const uint8_t handshake_hash[64], int initiator, uint8_t header[59]);
/* Ordered bytes, independent of UDP packet boundaries. May consume a prefix
 * of input: caller retains the remainder, including when a message is pending.
 * plain/plain_size are readable ONLY after MESSAGE, until release/clear.
 * Empty authenticated keepalives are consumed without MESSAGE. */
hdt_record_result hdt_record_feed(hdt_record *r, const uint8_t *data, size_t size, size_t *used);
void hdt_record_release(hdt_record *r);
/* Writes one complete framed record. Caller retains it until transport ACK;
 * this layer owns no TX queue. Zero return does not advance the crypto state. */
size_t hdt_record_seal(hdt_record *r, const uint8_t *plain, size_t size,
                        uint8_t *out, size_t capacity);
/* Wipes all keys, crypto state and buffers. A new init is required afterward. */
void hdt_record_clear(hdt_record *r);
#ifdef __cplusplus
}
#endif
#endif
