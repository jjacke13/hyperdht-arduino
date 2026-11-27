/* SPDX-License-Identifier: LGPL-3.0-only
 * Fixed-buffer adaptation of hyperdht-cpp compact/messages; see PROVENANCE.md. */
#ifndef HDTINY_RPC_H
#define HDTINY_RPC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { HDT_RPC_REQUEST = 3, HDT_RPC_RESPONSE = 0x13, HDT_RPC_MAX_DATAGRAM = 2048 };
enum { HDT_HAS_ID = 1, HDT_HAS_TOKEN = 2, HDT_INTERNAL = 4,
       HDT_HAS_TARGET = 8, HDT_HAS_VALUE = 16,
       HDT_HAS_CLOSER = 4, HDT_HAS_ERROR = 8 };
typedef struct { uint8_t ip[4]; uint16_t port; } hdt_addr;
typedef struct { const uint8_t *data; size_t size; } hdt_slice;
/* Decoded views borrow the input datagram; never retain them past its lifetime. */
typedef struct {
    uint8_t type, flags;
    uint16_t tid;
    hdt_addr observed;
    const uint8_t *id, *token, *target;
    uint32_t command, error;
    hdt_slice value, closer;
} hdt_rpc;

int hdt_addr_equal(const hdt_addr *a, const hdt_addr *b);
/* Encoders return zero on insufficient space/invalid input. Ignore partial output. */
size_t hdt_rpc_encode(const hdt_rpc *m, uint8_t *out, size_t capacity);
/* A failure leaves out zeroed. Rejects unknown flags and trailing envelope bytes. */
int hdt_rpc_decode(const uint8_t *data, size_t size, hdt_rpc *out);
/* closer is a validated array payload, with six bytes per IPv4 address. */
int hdt_rpc_closer(const hdt_rpc *m, size_t index, hdt_addr *out);
/* FIND_PEER values: exact requested key, bounded relay copy, no retained views.
 * Rejects trailing bytes, zero ports and a relay count greater than capacity. */
int hdt_peer_record(hdt_slice value, const uint8_t key[32],
                    hdt_addr *relays, size_t capacity, size_t *count);
#ifdef __cplusplus
}
#endif
#endif
