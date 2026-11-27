/* SPDX-License-Identifier: Apache-2.0 */
#include "hdtiny/udx_rx.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void invariant(const hdt_udx_rx_t *rx) {
    size_t i, j, free_count = 0;
    uint32_t credit = hdt_udx_rx_credit(rx);
    assert(credit == 0 || credit == 1452);
    /* Stock send_rwnd_in_packets() at IPv4 MTU 1200 and 1500. */
    assert(!credit || (credit / (1200 - 48) == 1 &&
                       credit / (1500 - 48) == 1));
    for (i = 0; i < HDT_UDX_RX_SLOTS; ++i) {
        const hdt_udx_rx_slot_t *s = &rx->slots[i];
        assert(s->length <= HDT_UDX_RX_PAYLOAD);
        if (!s->length) { ++free_count; continue; }
        assert(s->seq != rx->ack);
        for (j = i + 1; j < HDT_UDX_RX_SLOTS; ++j)
            assert(!rx->slots[j].length || rx->slots[j].seq != s->seq);
    }
    assert(credit == (free_count ? 1452u : 0u));
}

static void permutations(void) {
    unsigned a, b, c, d, k, wrap;
    for (wrap = 0; wrap < 2; ++wrap) {
        uint32_t start = wrap ? UINT32_MAX - 1 : 100;
        for (a = 0; a < 4; ++a) for (b = 0; b < 4; ++b)
        for (c = 0; c < 4; ++c) for (d = 0; d < 4; ++d) {
            unsigned order[4];
            hdt_udx_rx_t rx;
            if (a == b || a == c || a == d || b == c || b == d || c == d)
                continue;
            order[0] = a; order[1] = b; order[2] = c; order[3] = d;
            hdt_udx_rx_init(&rx, start);
            for (k = 0; k < 4; ++k) {
                uint8_t value = (uint8_t)order[k];
                assert(hdt_udx_rx_admit(&rx, start + value, &value, 1) ==
                       HDT_UDX_RX_RETAINED);
                invariant(&rx);
            }
            assert(rx.ack == start + 4);
            assert(hdt_udx_rx_sack_mask(&rx) == 0);
            for (k = 0; k < 4; ++k) {
                const hdt_udx_rx_slot_t *s = hdt_udx_rx_peek(&rx);
                assert(s && s->seq == start + k && s->data[0] == k);
                assert(hdt_udx_rx_release(&rx, s->seq));
            }
            assert(!hdt_udx_rx_peek(&rx));
            assert(hdt_udx_rx_credit(&rx) == 1452);
        }
    }
}

static void gap_and_duplicates(void) {
    hdt_udx_rx_t rx, before;
    uint8_t value = 7;
    unsigned i;
    hdt_udx_rx_init(&rx, UINT32_MAX);
    for (i = 1; i < 4; ++i)
        assert(hdt_udx_rx_admit(&rx, UINT32_MAX + i, &value, 1) ==
               HDT_UDX_RX_RETAINED);
    assert(rx.ack == UINT32_MAX);
    assert(hdt_udx_rx_sack_mask(&rx) == 14);
    assert(hdt_udx_rx_credit(&rx) == 1452); /* Keep gap retransmission possible. */
    assert(!hdt_udx_rx_peek(&rx));
    before = rx;
    value = 9;
    assert(hdt_udx_rx_admit(&rx, 0, &value, 1) == HDT_UDX_RX_DUPLICATE);
    assert(hdt_udx_rx_admit(&rx, 3, &value, 1) == HDT_UDX_RX_DROPPED);
    assert(!hdt_udx_rx_release(&rx, 0));
    assert(memcmp(&rx, &before, sizeof(rx)) == 0);
    assert(hdt_udx_rx_admit(&rx, UINT32_MAX, &value, 1) == HDT_UDX_RX_RETAINED);
    assert(rx.ack == 3);
    before = rx;
    assert(hdt_udx_rx_admit(&rx, UINT32_MAX, &value, 1) == HDT_UDX_RX_DUPLICATE);
    assert(memcmp(&rx, &before, sizeof(rx)) == 0);
    assert(hdt_udx_rx_release(&rx, UINT32_MAX));
    assert(hdt_udx_rx_credit(&rx) == 1452);
    assert(hdt_udx_rx_peek(&rx)->data[0] == 7);
    assert(hdt_udx_rx_release(&rx, 0));
    assert(hdt_udx_rx_credit(&rx) == 1452);
    invariant(&rx);
}

static void bounded_burst(void) {
    hdt_udx_rx_t rx, before;
    uint8_t payload[HDT_UDX_RX_PAYLOAD];
    unsigned seq;
    memset(payload, 0xa5, sizeof(payload));
    hdt_udx_rx_init(&rx, 0);
    /* Peer initially believes 4MiB credit: receiver never trusts that bound. */
    for (seq = 0; seq < 10000; ++seq) {
        hdt_udx_rx_result_t result = hdt_udx_rx_admit(&rx, seq, payload,
                                                    sizeof(payload));
        assert(result == (seq < 4 ? HDT_UDX_RX_RETAINED : HDT_UDX_RX_DROPPED));
        assert(rx.ack == (seq < 4 ? seq + 1 : 4));
        invariant(&rx);
    }
    before = rx;
    assert(hdt_udx_rx_admit(&rx, 4, payload, 1) == HDT_UDX_RX_DROPPED);
    assert(memcmp(&rx, &before, sizeof(rx)) == 0);
    assert(hdt_udx_rx_release(&rx, 0));
    assert(hdt_udx_rx_credit(&rx) == 1452);
    assert(hdt_udx_rx_release(&rx, 1));
    assert(hdt_udx_rx_credit(&rx) == 1452);
    assert(hdt_udx_rx_admit(&rx, 4, payload, sizeof(payload)) == HDT_UDX_RX_RETAINED);
    assert(rx.ack == 5);
    for (seq = 2; seq < 5; ++seq) {
        const hdt_udx_rx_slot_t *s = hdt_udx_rx_peek(&rx);
        assert(s && s->seq == seq && s->length == sizeof(payload));
        assert(memcmp(s->data, payload, sizeof(payload)) == 0);
        assert(hdt_udx_rx_release(&rx, seq));
    }
}

static void invalid(void) {
    hdt_udx_rx_t rx, before;
    uint8_t byte = 1;
    hdt_udx_rx_init(&rx, 23);
    before = rx;
    assert(hdt_udx_rx_admit(&rx, 23, NULL, 1) == HDT_UDX_RX_INVALID);
    assert(hdt_udx_rx_admit(&rx, 23, &byte, 0) == HDT_UDX_RX_INVALID);
    assert(hdt_udx_rx_admit(&rx, 23, &byte, 1453) == HDT_UDX_RX_INVALID);
    assert(hdt_udx_rx_admit(&rx, 23, &byte, SIZE_MAX) == HDT_UDX_RX_INVALID);
    assert(hdt_udx_rx_admit(&rx, 23 + UINT32_C(0x80000000), &byte, 1) ==
           HDT_UDX_RX_DROPPED);
    assert(!hdt_udx_rx_release(&rx, 23));
    assert(memcmp(&rx, &before, sizeof(rx)) == 0);
}

static void reserve_missing_packet(void) {
    hdt_udx_rx_t rx, before;
    uint8_t byte = 42;
    hdt_udx_rx_init(&rx, 0);
    assert(hdt_udx_rx_admit(&rx, 0, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(hdt_udx_rx_admit(&rx, 2, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(hdt_udx_rx_admit(&rx, 3, &byte, 1) == HDT_UDX_RX_RETAINED);
    before = rx;
    assert(hdt_udx_rx_admit(&rx, 4, &byte, 1) == HDT_UDX_RX_DROPPED);
    assert(memcmp(&rx, &before, sizeof(rx)) == 0);
    assert(hdt_udx_rx_sack_mask(&rx) == 6);
    /* Retransmission of the missing sequence can fill the reserved slot. */
    assert(hdt_udx_rx_admit(&rx, 1, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(rx.ack == 4);
    invariant(&rx);
}

static void successive_gaps(void) {
    hdt_udx_rx_t rx;
    uint8_t byte = 42;
    hdt_udx_rx_init(&rx, 0);
    assert(hdt_udx_rx_admit(&rx, 0, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(hdt_udx_rx_admit(&rx, 3, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(hdt_udx_rx_admit(&rx, 4, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(hdt_udx_rx_admit(&rx, 1, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(rx.ack == 2 && hdt_udx_rx_credit(&rx) == 0);
    /* Full need not be contiguous, but always has ordered data to release. */
    assert(hdt_udx_rx_peek(&rx)->seq == 0);
    assert(hdt_udx_rx_release(&rx, 0));
    assert(hdt_udx_rx_credit(&rx) == 1452);
    assert(hdt_udx_rx_admit(&rx, 2, &byte, 1) == HDT_UDX_RX_RETAINED);
    assert(rx.ack == 5);
    invariant(&rx);
}

static void fragmented_record(void) {
    static const size_t fragments[] = {1, 17, 522, 1044};
    size_t f;
    for (f = 0; f < sizeof(fragments) / sizeof(fragments[0]); ++f) {
        hdt_udx_rx_t rx;
        uint8_t record[1044], accumulated[1044];
        size_t sent = 0, copied = 0, turns = 0;
        uint32_t seq = UINT32_MAX - 2;
        memset(record, 0x6b, sizeof(record));
        hdt_udx_rx_init(&rx, seq);
        while (copied < sizeof(record)) {
            assert(++turns < 3000);
            if (sent < sizeof(record) && hdt_udx_rx_credit(&rx)) {
                size_t length = sizeof(record) - sent;
                if (length > fragments[f]) length = fragments[f];
                assert(hdt_udx_rx_admit(&rx, seq++, record + sent, length) ==
                       HDT_UDX_RX_RETAINED);
                sent += length;
            } else {
                const hdt_udx_rx_slot_t *s = hdt_udx_rx_peek(&rx);
                assert(s && s->length <= sizeof(accumulated) - copied);
                memcpy(accumulated + copied, s->data, s->length);
                copied += s->length;
                assert(hdt_udx_rx_release(&rx, s->seq));
            }
            invariant(&rx);
        }
        assert(memcmp(record, accumulated, sizeof(record)) == 0);
        assert(!hdt_udx_rx_peek(&rx));
    }
}

int hdt_test_udx_rx(void) {
    permutations();
    gap_and_duplicates();
    bounded_burst();
    invalid();
    reserve_missing_packet();
    successive_gaps();
    fragmented_record();
    printf("udx_rx: slot=%zu pool=%zu payload=%u slots=%u; tests passed\n",
           sizeof(hdt_udx_rx_slot_t), sizeof(hdt_udx_rx_t),
           HDT_UDX_RX_PAYLOAD, HDT_UDX_RX_SLOTS);
    return 0;
}

#ifdef HDT_UDX_RX_TEST_MAIN
int main(void) { return hdt_test_udx_rx(); }
#endif
