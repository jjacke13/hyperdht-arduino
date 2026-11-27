/* SPDX-License-Identifier: Apache-2.0 */
#include "hdtiny/transport.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>

static const hdt_addr peer = {{127,0,0,1}, 12345};
static void le(uint8_t *p, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
}
static void packet(uint8_t *p, unsigned flags, uint32_t seq, uint32_t ack) {
    memset(p, 0, 64); p[0] = 255; p[1] = 1; p[2] = (uint8_t)flags;
    le(p + 4, 1); le(p + 8, 1452); le(p + 12, seq); le(p + 16, ack);
    p[20] = 42;
}
static void invalid(void) {
    hdt_transport t, before;
    uint8_t p[64]; hdt_addr other = peer;
    hdt_transport_init(&t, &peer, 1, 2, 0, 0); before = t;
    packet(p, 1, 0, 0);
    ++other.port;
    assert(!hdt_transport_receive(&t, &other, p, 21));
    assert(!hdt_transport_receive(&t, &peer, p, 19));
    p[0] = 254; assert(!hdt_transport_receive(&t, &peer, p, 21)); p[0] = 255;
    p[1] = 2; assert(!hdt_transport_receive(&t, &peer, p, 21)); p[1] = 1;
    p[4] = 2; assert(!hdt_transport_receive(&t, &peer, p, 21)); p[4] = 1;
    p[3] = 2; assert(!hdt_transport_receive(&t, &peer, p, 21)); p[3] = 0;
    p[2] = 64; assert(!hdt_transport_receive(&t, &peer, p, 21)); p[2] = 1;
    assert(!hdt_transport_receive(&t, &peer, p, 20));
    packet(p, 4, 0, 0); assert(!hdt_transport_receive(&t, &peer, p, 27));
    packet(p, 1, 0, 1); assert(!hdt_transport_receive(&t, &peer, p, 21));
    assert(!memcmp(&t, &before, sizeof(t)));
}
static void endings(void) {
    hdt_transport t;
    uint8_t p[64];
    hdt_transport_init(&t, &peer, 1, 2, 0, 0);
    packet(p, 3, 0, 0); /* Combined DATA/END. */
    assert(hdt_transport_receive(&t, &peer, p, 21));
    assert(t.incoming.ack == 1 && t.remote_end == 1);
    assert(hdt_udx_rx_peek(&t.incoming));
    assert(hdt_udx_rx_release(&t.incoming, 0)); hdt_transport_consumed(&t);
    assert(t.remote_end == 2);
    assert(hdt_transport_receive(&t, &peer, p, 21)); /* Duplicate END must stay EOF. */
    assert(t.remote_end == 2);
    hdt_transport_init(&t, &peer, 1, 2, 0, 0);
    packet(p, 2, 1, 0); /* END arrives before preceding DATA. */
    assert(hdt_transport_receive(&t, &peer, p, 20));
    assert(t.remote_end == 1 && t.incoming.ack == 0);
    packet(p, 1, 0, 0); assert(hdt_transport_receive(&t, &peer, p, 21));
    assert(t.incoming.ack == 2);
    assert(hdt_udx_rx_release(&t.incoming, 0)); hdt_transport_consumed(&t);
    assert(t.remote_end == 2 && !hdt_udx_rx_peek(&t.incoming));
    packet(p, 1, 1, 0); assert(!hdt_transport_receive(&t, &peer, p, 21));
    hdt_transport_init(&t, &peer, 1, 2, 0, 0);
    packet(p, 1, 2, 0); assert(hdt_transport_receive(&t, &peer, p, 21));
    packet(p, 2, 1, 0); assert(!hdt_transport_receive(&t, &peer, p, 20));
    hdt_transport_init(&t, &peer, 1, 2, 0, UINT32_MAX);
    packet(p, 2, UINT32_MAX, 0); assert(hdt_transport_receive(&t, &peer, p, 20));
    assert(t.remote_end == 2 && t.incoming.ack == 0 && t.incoming.next_read == 0);
}
static void timers(void) {
    hdt_transport t;
    uint8_t p[64], out[1472], byte = 42;
    uint64_t now = 0;
    hdt_transport_init(&t, &peer, 1, 2, 0, 0);
    assert(hdt_transport_write(&t, &byte, 1));
    assert(!hdt_transport_write(&t, &byte, 1));
    assert(!hdt_transport_end(&t));
    assert(!hdt_transport_poll(&t, now, out, 20) && !t.sent);
    assert(hdt_transport_poll(&t, now, out, sizeof(out)) == 21);
    assert(!hdt_transport_poll(&t, 999, out, sizeof(out)));
    for (unsigned i = 0; i < 6; ++i) {
        now += t.rto;
        assert(hdt_transport_poll(&t, now, out, sizeof(out)) == 21);
        assert(out[20] == byte && t.retries == i + 1);
    }
    now += t.rto;
    assert(!hdt_transport_poll(&t, now, out, sizeof(out)) && t.closed);
    hdt_transport_init(&t, &peer, 1, 2, 1, 0);
    packet(p, 1, 0, 0); /* Old ACK must not discard new receive DATA. */
    assert(hdt_transport_receive(&t, &peer, p, 21));
    assert(t.incoming.ack == 1);
    packet(p, 0, 0, 1); le(p + 8, 0);
    assert(hdt_transport_receive(&t, &peer, p, 20));
    assert(hdt_transport_write(&t, &byte, 1));
    assert(hdt_transport_poll(&t, 0, out, sizeof(out)) == 20); /* Only queued ACK. */
    assert(!t.sent);
    assert(hdt_transport_poll(&t, 1000, out, sizeof(out)) == 21); /* Bounded ZWP. */
    packet(p, 16, 0, 1); assert(hdt_transport_receive(&t, &peer, p, 20));
    assert(t.closed);
}
static void combined_end_backlog(void) {
    for (unsigned wrap = 0; wrap < 2; ++wrap) for (unsigned reordered = 0; reordered < 2; ++reordered) {
        hdt_transport t;
        uint8_t a[64], b[64];
        uint32_t start = wrap ? UINT32_MAX : 0;
        hdt_transport_init(&t, &peer, 1, 2, 0, start);
        packet(a, 1, start, 0); a[20] = 'A';
        packet(b, 3, start + 1, 0); b[20] = 'B';
        assert(hdt_transport_receive(&t, &peer, reordered ? b : a, 21));
        assert(hdt_transport_receive(&t, &peer, reordered ? a : b, 21));
        assert(t.incoming.ack == start + 2);
        assert(hdt_udx_rx_peek(&t.incoming)->data[0] == 'A');
        assert(hdt_udx_rx_release(&t.incoming, start)); hdt_transport_consumed(&t);
        assert(t.remote_end == 1);
        const hdt_udx_rx_slot_t *slot = hdt_udx_rx_peek(&t.incoming);
        assert(slot && slot->data[0] == 'B');
        assert(hdt_udx_rx_release(&t.incoming, start + 1)); hdt_transport_consumed(&t);
        assert(t.remote_end == 2 && !hdt_udx_rx_peek(&t.incoming));
    }
}
static void stale_window(void) {
    hdt_transport t;
    uint8_t p[64];
    hdt_transport_init(&t, &peer, 1, 2, 0, 0);
    packet(p, 0, 7, 0); le(p + 8, 1452);
    assert(hdt_transport_receive(&t, &peer, p, 20));
    packet(p, 0, 6, 0); le(p + 8, 1);
    assert(hdt_transport_receive(&t, &peer, p, 20));
    assert(t.peer_credit == 1452);
}
int main(void) { invalid(); endings(); timers(); combined_end_backlog(); stale_window(); return 0; }
