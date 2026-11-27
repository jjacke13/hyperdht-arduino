/* SPDX-License-Identifier: LGPL-3.0-only
 * Fixed-buffer adaptation of hyperdht-cpp/src/secret_stream.cpp. */
#include "hdtiny/record.h"
#include <string.h>

/* Original hyperswarm/secret-stream namespace values, unchanged. */
static const uint8_t ns[2][32] = {
    {0x74,0x2c,0x9d,0x83,0x3d,0x43,0x0a,0xf4,0xc4,0x8a,0x87,0x05,0xe9,0x16,0x31,0xee,
     0xcf,0x29,0x54,0x42,0xbb,0xca,0x18,0x99,0x6e,0x59,0x70,0x97,0x72,0x3b,0x10,0x61},
    {0xa9,0x31,0xa0,0x15,0x5b,0x5c,0x09,0xe6,0xd2,0x86,0x28,0x23,0x6a,0xf8,0x3c,0x4b,
     0x8a,0x6a,0xf9,0xaf,0x60,0x98,0x6e,0xde,0xed,0xe9,0xdc,0x5d,0x63,0x19,0x2b,0xf7}
};
static void prefix(uint8_t *out, size_t n) {
    out[0] = (uint8_t)n; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)(n >> 16);
}
void hdt_record_clear(hdt_record *r) { if (r) sodium_memzero(r, sizeof(*r)); }
static hdt_record_result fail(hdt_record *r) {
    hdt_record_clear(r); r->failed = 1;
    return HDT_RECORD_ERROR;
}
int hdt_record_init(hdt_record *r, const uint8_t tx_key[32], const uint8_t rx_key[32],
                    const uint8_t hash[64], int initiator, uint8_t header[59]) {
    if (!r) return 0;
    hdt_record_clear(r);
    if (!tx_key || !rx_key || !hash || !header) return 0;
    initiator = !!initiator;
    prefix(header, 56);
    if (crypto_generichash(header + 3, 32, ns[initiator], 32, hash, 64) ||
        crypto_generichash(r->expected_id, 32, ns[!initiator], 32, hash, 64) ||
        crypto_secretstream_xchacha20poly1305_init_push(&r->tx, header + 35, tx_key)) {
        fail(r); return 0;
    }
    memcpy(r->rx_key, rx_key, 32);
    r->active = 1;
    return 1;
}
hdt_record_result hdt_record_feed(hdt_record *r, const uint8_t *data, size_t size, size_t *used) {
    if (used) *used = 0;
    if (!r || !used || !r->active || r->failed) return HDT_RECORD_ERROR;
    if (!data && size) return fail(r);
    if (r->pending) return HDT_RECORD_MESSAGE;
    while (*used < size) {
        if (r->prefix_used < 3) {
            r->prefix[r->prefix_used++] = data[(*used)++];
            if (r->prefix_used < 3) continue;
            r->body_size = (size_t)r->prefix[0] | (size_t)r->prefix[1] << 8 | (size_t)r->prefix[2] << 16;
            if ((!r->header_received && r->body_size != 56) ||
                (r->header_received && (r->body_size < 17 || r->body_size > HDT_RECORD_CIPHER_MAX)))
                return fail(r);
        }
        size_t n = r->body_size - r->body_used;
        if (n > size - *used) n = size - *used;
        memcpy(r->cipher + r->body_used, data + *used, n);
        r->body_used += n; *used += n;
        if (r->body_used != r->body_size) continue;
        if (!r->header_received) {
            if (sodium_memcmp(r->expected_id, r->cipher, 32) ||
                crypto_secretstream_xchacha20poly1305_init_pull(&r->rx, r->cipher + 32, r->rx_key))
                return fail(r);
            sodium_memzero(r->rx_key, 32);
            r->header_received = 1;
        } else {
            unsigned long long length = 0;
            unsigned char tag = 0;
            if (crypto_secretstream_xchacha20poly1305_pull(&r->rx, r->plain, &length, &tag,
                    r->cipher, r->body_size, NULL, 0)) return fail(r);
            /* Match original SecretStream::decrypt: sodium handles tags/rekeying. */
            r->plain_size = (size_t)length;
            r->pending = length != 0;
        }
        r->prefix_used = 0; r->body_size = r->body_used = 0;
        if (r->pending) return HDT_RECORD_MESSAGE;
    }
    return HDT_RECORD_INPUT;
}
void hdt_record_release(hdt_record *r) {
    if (!r) return;
    sodium_memzero(r->plain, sizeof(r->plain));
    r->plain_size = 0; r->pending = 0;
}
size_t hdt_record_seal(hdt_record *r, const uint8_t *plain, size_t size,
                        uint8_t *out, size_t capacity) {
    if (!r || !r->active || !r->header_received || r->failed || !out ||
        (!plain && size) || size > HDT_RECORD_PLAIN_MAX || capacity < size + 20) return 0;
    unsigned long long n = 0;
    if (crypto_secretstream_xchacha20poly1305_push(&r->tx, out + 3, &n, plain, size,
            NULL, 0, crypto_secretstream_xchacha20poly1305_TAG_MESSAGE)) return 0;
    prefix(out, (size_t)n);
    return (size_t)n + 3;
}
