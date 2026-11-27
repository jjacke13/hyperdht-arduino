/* SPDX-License-Identifier: Apache-2.0
 * Adapted wire header/sequence/lifecycle rules from libudx
 * 759bf7641347f65a1896d4bc4c3b1606a459f466; Copyright 2021 Holepunch Inc.
 * Modified: fixed-buffer stop-and-wait sender and cooperative poll timers.
 */
#include "hdtiny/transport.h"
#include <string.h>

enum { DATA = 1, END = 2, SACK = 4, MESSAGE = 8, DESTROY = 16, HEARTBEAT = 32 };
static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
void hdt_transport_clear(hdt_transport *t) {
    /* Ciphertext only; caller separately wipes Noise/record key material. */
    memset(t, 0, sizeof(*t)); t->closed = 1;
}
void hdt_transport_init(hdt_transport *t, const hdt_addr *peer, uint32_t local_id,
                         uint32_t remote_id, uint32_t send_seq, uint32_t recv_seq) {
    memset(t, 0, sizeof(*t));
    t->peer = *peer; t->local_id = local_id; t->remote_id = remote_id;
    t->seq = send_seq; t->peer_credit = 1152; t->rto = 1000;
    t->window_seq = recv_seq; t->window_ack = send_seq;
    hdt_udx_rx_init(&t->incoming, recv_seq);
}
int hdt_transport_write(hdt_transport *t, const uint8_t *data, size_t size) {
    if (t->closed || t->queued || t->local_end || !data || !size || size > sizeof(t->tx)) return 0;
    if (data != t->tx) memcpy(t->tx, data, size);
    t->tx_size = size;
    t->queued = 1; t->tx_end = 0; t->sent = 0; t->retries = 0; t->rto = 1000;
    return 1;
}
int hdt_transport_end(hdt_transport *t) {
    if (t->closed || t->queued || t->local_end) return 0;
    t->queued = 1; t->tx_end = 1; t->tx_size = 0;
    t->sent = 0; t->retries = 0; t->rto = 1000;
    return 1;
}
static void advance_end(hdt_transport *t) {
    if (t->remote_end == 1 && t->incoming.ack == t->end_seq)
        ++t->incoming.ack;
    if (t->remote_end == 1 && !t->end_data && t->incoming.next_read == t->end_seq &&
        t->incoming.ack == t->end_seq + 1 && !hdt_udx_rx_peek(&t->incoming)) {
        ++t->incoming.next_read; t->remote_end = 2;
    }
}
void hdt_transport_consumed(hdt_transport *t) {
    if (t->remote_end == 1 && !t->end_data && t->incoming.next_read == t->end_seq &&
        t->incoming.ack == t->end_seq + 1) {
        ++t->incoming.next_read;
        t->remote_end = 2;
    }
    if (t->remote_end == 1 && t->incoming.next_read == t->incoming.ack &&
        t->incoming.ack == t->end_seq + 1) t->remote_end = 2;
    t->ack_due = 1;
}
size_t hdt_transport_poll(hdt_transport *t, uint64_t now, uint8_t *out, size_t capacity) {
    uint8_t flags = 0;
    size_t size = 20;
    int send = t->queued && (!t->sent || now - t->last_send >= t->rto);
    uint32_t seq = t->seq;
    if (t->closed || !out || capacity < 20) return 0;
    if (send) {
        if (capacity < 20 + t->tx_size) return 0;
        if (t->sent && t->retries == 6) { t->closed = 1; return 0; }
        if (!t->sent && !t->peer_credit) {
            /* One pending-data zero-window probe after one RTO, as in libudx. */
            if (now - t->last_send < t->rto) send = 0;
        }
        if (!t->sent && t->peer_credit && t->peer_credit < 1152) send = 0;
        if (send) {
            flags = t->tx_end ? END : DATA;
            if (t->sent) {
                ++t->retries; t->rto = t->rto < 15000 ? t->rto * 2 : 30000;
            }
            t->sent = 1; t->last_send = now;
            memcpy(out + 20, t->tx, t->tx_size); size += t->tx_size;
        }
    }
    if (!send && !t->ack_due) return 0;
    out[0] = 255; out[1] = 1; out[2] = flags; out[3] = 0;
    write32(out + 4, t->remote_id);
    write32(out + 8, hdt_udx_rx_credit(&t->incoming));
    /* DATA/END uses outstanding seq; pure ACK uses next unsent seq. */
    write32(out + 12, seq + (t->queued && t->sent && !send ? 1u : 0u));
    write32(out + 16, t->incoming.ack);
    t->ack_due = 0;
    return size;
}
int hdt_transport_receive(hdt_transport *t, const hdt_addr *source,
                           const uint8_t *packet, size_t size) {
    uint8_t flags, offset;
    uint32_t seq, ack, distance;
    size_t length, sack_size;
    hdt_udx_rx_result_t result = HDT_UDX_RX_DROPPED;
    if (t->closed || !packet || !hdt_addr_equal(source, &t->peer) ||
        size < 20 || size > HDT_TRANSPORT_DATAGRAM_MAX || packet[0] != 255 ||
        packet[1] != 1 || read32(packet + 4) != t->local_id) return 0;
    flags = packet[2]; offset = packet[3];
    if ((flags & ~63u) || offset > size - 20 ||
        ((flags & MESSAGE) && (flags & (DATA | END | SACK))) ||
        ((flags & DESTROY) && (flags & ~DESTROY)) ||
        ((flags & HEARTBEAT) && (flags & ~HEARTBEAT))) return 0;
    length = size - 20 - offset;
    sack_size = offset ? offset : size - 20;
    if ((flags & SACK) && (sack_size % 8 || ((flags & (DATA | END)) && !offset))) return 0;
    if (!(flags & (DATA | MESSAGE | SACK | HEARTBEAT)) && length) return 0;
    if ((flags & DATA) && (!length || length > HDT_UDX_RX_PAYLOAD)) return 0;
    seq = read32(packet + 12); ack = read32(packet + 16);
    if (t->remote_end && (flags & (DATA | END)) &&
        seq - t->end_seq < UINT32_C(0x80000000) && seq != t->end_seq) return 0;
    if (t->remote_end && seq == t->end_seq && (flags & DATA) && !t->end_data) return 0;
    if ((flags & END) && !t->remote_end) for (size_t i = 0; i < HDT_UDX_RX_SLOTS; ++i) {
        const hdt_udx_rx_slot_t *slot = &t->incoming.slots[i];
        if (slot->length && slot->seq != seq &&
            slot->seq - seq < UINT32_C(0x80000000)) return 0;
    }
    /* Stop-and-wait retains SACKed TX until cumulative ACK; never loop ranges. */
    if (flags & SACK) for (size_t i = 0; i < sack_size; i += 8) {
        uint32_t a = read32(packet + 20 + i), b = read32(packet + 24 + i);
        if (!((b - a) && b - a < UINT32_C(0x80000000))) return 0;
    }
    if (flags & DESTROY) { t->closed = 1; return 1; }
    if (flags & HEARTBEAT) { t->ack_due = 1; return 1; }
    if (ack - t->seq == UINT32_C(0x80000000)) return 0;
    if (ack - t->seq < UINT32_C(0x80000000) &&
        ack - t->seq > (uint32_t)(t->queued && t->sent)) return 0;
    if ((ack == t->seq || (t->queued && t->sent && ack == t->seq + 1)) &&
        ((seq != t->window_seq && seq - t->window_seq < UINT32_C(0x80000000)) ||
         (seq == t->window_seq && ack - t->window_ack < UINT32_C(0x80000000)))) {
        t->peer_credit = read32(packet + 8);
        t->window_seq = seq; t->window_ack = ack;
    }
    if (t->queued && t->sent && ack == t->seq + 1) {
        ++t->seq; t->queued = 0; t->sent = 0; t->tx_size = 0;
        if (t->tx_end) t->local_end = 1;
    }
    if (flags & (DATA | END)) {
        distance = seq - t->incoming.ack;
        if (flags & DATA) result = hdt_udx_rx_admit(&t->incoming, seq, packet + 20 + offset, length);
        if ((flags & END) && !t->remote_end && distance < HDT_UDX_RX_SLOTS &&
            (!(flags & DATA) || result == HDT_UDX_RX_RETAINED)) {
            t->end_seq = seq; t->remote_end = 1; t->end_data = !!(flags & DATA);
        }
        advance_end(t); t->ack_due = 1;
    }
    return 1;
}
