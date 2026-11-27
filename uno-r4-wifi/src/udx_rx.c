/* SPDX-License-Identifier: Apache-2.0
 * Adapted from libudx; Copyright 2021 Holepunch Inc.
 * Modified: fixed slots, admission-before-ACK and conservative receive credit.
 * Exact source/provenance and unimplemented protocol layers: docs/UDX-RX.md.
 */
#include "hdtiny/udx_rx.h"

#include <string.h>

static hdt_udx_rx_slot_t *find(hdt_udx_rx_t *rx, uint32_t seq) {
    size_t i;
    for (i = 0; i < HDT_UDX_RX_SLOTS; ++i)
        if (rx->slots[i].length && rx->slots[i].seq == seq)
            return &rx->slots[i];
    return NULL;
}

void hdt_udx_rx_init(hdt_udx_rx_t *rx, uint32_t initial_seq) {
    memset(rx, 0, sizeof(*rx));
    rx->ack = rx->next_read = initial_seq;
}

hdt_udx_rx_result_t hdt_udx_rx_admit(hdt_udx_rx_t *rx, uint32_t seq,
                                  const uint8_t *data, size_t length) {
    uint32_t distance = seq - rx->ack;
    size_t i, free_count = 0;
    hdt_udx_rx_slot_t *slot = NULL;
    if (!data || !length || length > HDT_UDX_RX_PAYLOAD)
        return HDT_UDX_RX_INVALID;
    /* libudx seq_diff semantics, using unsigned arithmetic for portable C99. */
    if (distance > UINT32_C(0x80000000))
        return HDT_UDX_RX_DUPLICATE;
    if (distance >= HDT_UDX_RX_SLOTS)
        return HDT_UDX_RX_DROPPED;
    if (find(rx, seq)) return HDT_UDX_RX_DUPLICATE;
    for (i = 0; i < HDT_UDX_RX_SLOTS; ++i) {
        if (!rx->slots[i].length) {
            slot = &rx->slots[i];
            ++free_count;
        }
    }
    /* Reserve a slot for the missing cumulative-ACK sequence. */
    if (!free_count || (distance && free_count == 1))
        return HDT_UDX_RX_DROPPED;
    memcpy(slot->data, data, length);
    slot->seq = seq;
    slot->length = (uint16_t)length;
    while (find(rx, rx->ack)) ++rx->ack;
    return HDT_UDX_RX_RETAINED;
}

const hdt_udx_rx_slot_t *hdt_udx_rx_peek(const hdt_udx_rx_t *rx) {
    size_t i;
    for (i = 0; i < HDT_UDX_RX_SLOTS; ++i)
        if (rx->slots[i].length && rx->slots[i].seq == rx->next_read)
            return &rx->slots[i];
    return NULL;
}

int hdt_udx_rx_release(hdt_udx_rx_t *rx, uint32_t seq) {
    hdt_udx_rx_slot_t *slot;
    if (seq != rx->next_read || !(slot = find(rx, seq))) return 0;
    slot->length = 0;
    ++rx->next_read;
    return 1;
}

uint32_t hdt_udx_rx_credit(const hdt_udx_rx_t *rx) {
    size_t i, free_count = 0;
    for (i = 0; i < HDT_UDX_RX_SLOTS; ++i)
        if (!rx->slots[i].length) ++free_count;
    /* shortcut: one-packet credit for IPv4 MTU 1200..1500; review for other MTUs. */
    return free_count ? HDT_UDX_RX_PAYLOAD : 0;
}

uint32_t hdt_udx_rx_sack_mask(const hdt_udx_rx_t *rx) {
    uint32_t mask = 0;
    size_t i;
    for (i = 0; i < HDT_UDX_RX_SLOTS; ++i) {
        uint32_t distance = rx->slots[i].seq - rx->ack;
        if (rx->slots[i].length && distance < HDT_UDX_RX_SLOTS)
            mask |= UINT32_C(1) << distance;
    }
    return mask;
}
