/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HDTINY_UDX_RX_H
#define HDTINY_UDX_RX_H

#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* IPv4 MTU 1500 minus IP(20), UDP(8), UDX(20). */
#define HDT_UDX_RX_PAYLOAD 1452u
#define HDT_UDX_RX_SLOTS 4u

typedef struct {
    uint32_t seq;
    uint16_t length; /* Zero means free; empty DATA/END are not supported here. */
    uint8_t data[HDT_UDX_RX_PAYLOAD];
} hdt_udx_rx_slot_t;

typedef struct {
    uint32_t ack; /* Next sequence not cumulatively retained. */
    uint32_t next_read;
    hdt_udx_rx_slot_t slots[HDT_UDX_RX_SLOTS];
} hdt_udx_rx_t;

typedef enum {
    HDT_UDX_RX_RETAINED,
    HDT_UDX_RX_DUPLICATE,
    HDT_UDX_RX_DROPPED,
    HDT_UDX_RX_INVALID
} hdt_udx_rx_result_t;

/* Single-threaded, non-reentrant. Init only before use or after stream teardown. */
void hdt_udx_rx_init(hdt_udx_rx_t *rx, uint32_t initial_seq);
/* Only nonempty, validated DATA payloads: caller must check wire/source/stream. */
hdt_udx_rx_result_t hdt_udx_rx_admit(hdt_udx_rx_t *rx, uint32_t seq,
                                  const uint8_t *data, size_t length);
/* Borrowed oldest ordered payload; valid until release or teardown. */
const hdt_udx_rx_slot_t *hdt_udx_rx_peek(const hdt_udx_rx_t *rx);
/* Release only after copying/consuming the entire peeked payload. */
int hdt_udx_rx_release(hdt_udx_rx_t *rx, uint32_t seq);
/* Recompute and send a window update after release, including while idle. */
uint32_t hdt_udx_rx_credit(const hdt_udx_rx_t *rx);
/* Internal bitmap: bit i denotes retained ack+i. NOT UDX SACK wire encoding. */
uint32_t hdt_udx_rx_sack_mask(const hdt_udx_rx_t *rx);

#ifdef __cplusplus
}
#endif
#endif
