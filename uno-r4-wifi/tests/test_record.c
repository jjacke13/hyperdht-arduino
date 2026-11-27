/* SPDX-License-Identifier: LGPL-3.0-only */
#include "hdtiny/record.h"
#include "hdtiny/udx_rx.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t k1[32] = {1}, k2[32] = {2}, hash[64] = {3};
static void pair(hdt_record *a, hdt_record *b) {
    uint8_t ah[59], bh[59]; size_t used;
    assert(hdt_record_init(a,k1,k2,hash,1,ah));
    assert(hdt_record_init(b,k2,k1,hash,0,bh));
    for (size_t i = 0; i < 59; ++i) {
        assert(hdt_record_feed(a,bh+i,1,&used) == HDT_RECORD_INPUT && used == 1);
        assert(hdt_record_feed(b,ah+i,1,&used) == HDT_RECORD_INPUT && used == 1);
    }
    assert(a->header_received && b->header_received);
}
static void fragmented(void) {
    uint8_t plain[1024], wire[1044];
    for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)i;
    /* Copy incrementally out of the RX pool BEFORE record authentication. */
    for (size_t fragment = 1; fragment <= 1044; fragment += fragment < 17 ? 16 : 1027) {
        hdt_record a, b;
        hdt_udx_rx_t rx; hdt_udx_rx_init(&rx, UINT32_MAX - 8);
        pair(&a,&b);
        size_t n = hdt_record_seal(&a,plain,sizeof(plain),wire,sizeof(wire));
        assert(n == sizeof(wire));
        size_t pos = 0;
        while (pos < n) {
            size_t len = n - pos < fragment ? n - pos : fragment;
            uint32_t seq = rx.ack;
            assert(hdt_udx_rx_admit(&rx,seq,wire+pos,len) == HDT_UDX_RX_RETAINED);
            const hdt_udx_rx_slot_t *slot = hdt_udx_rx_peek(&rx);
            assert(slot && slot->seq == seq);
            size_t used;
            hdt_record_result result = hdt_record_feed(&b,slot->data,slot->length,&used);
            assert(used == len && result != HDT_RECORD_ERROR);
            pos += used;
            assert(result == (pos == n ? HDT_RECORD_MESSAGE : HDT_RECORD_INPUT));
            assert(hdt_udx_rx_release(&rx,seq));
        }
        assert(b.plain_size == 1024 && !memcmp(b.plain,plain,1024));
        size_t used = 999;
        assert(hdt_record_feed(&b,wire,n,&used) == HDT_RECORD_MESSAGE && used == 0);
        hdt_record_release(&b);
        assert(!b.pending && !b.plain_size);
        for (size_t i = 0; i < sizeof(b.plain); ++i) assert(b.plain[i] == 0);
        hdt_record_clear(&a); hdt_record_clear(&b);
    }
}
static void boundaries(void) {
    hdt_record a,b;
    uint8_t wire[2100], plain[1025] = {42}; size_t used;
    pair(&a,&b);
    hdt_record before = a;
    assert(!hdt_record_seal(&a,plain,1025,wire,sizeof(wire)));
    assert(!memcmp(&before,&a,sizeof(a)));
    assert(!hdt_record_seal(&a,plain,1024,wire,1043));
    assert(!memcmp(&before,&a,sizeof(a)));
    size_t n = hdt_record_seal(&a,NULL,0,wire,sizeof(wire));
    assert(n == 20 && hdt_record_feed(&b,wire,n,&used) == HDT_RECORD_INPUT && used == n);
    n = hdt_record_seal(&a,plain,1,wire,sizeof(wire));
    n += hdt_record_seal(&a,plain,1,wire+n,sizeof(wire)-n);
    assert(hdt_record_feed(&b,wire,n,&used) == HDT_RECORD_MESSAGE && used == 21);
    assert(b.plain_size == 1 && b.plain[0] == 42);
    hdt_record_release(&b);
    assert(hdt_record_feed(&b,wire+21,n-21,&used) == HDT_RECORD_MESSAGE && used == 21);
    hdt_record_release(&b);
    /* Reject a 24-bit oversized record as soon as its 3-byte prefix arrives. */
    uint8_t oversized[] = {0xff,0xff,0xff};
    assert(hdt_record_feed(&b,oversized,3,&used) == HDT_RECORD_ERROR && used == 3);
    assert(!b.active && b.failed && !b.pending);
    assert(!hdt_record_seal(&b,plain,1,wire,sizeof(wire)));
    pair(&a,&b);
    n = hdt_record_seal(&a,plain,1024,wire,sizeof(wire));
    wire[n-1] ^= 1;
    assert(hdt_record_feed(&b,wire,n-1,&used) == HDT_RECORD_INPUT);
    assert(!b.pending);
    assert(hdt_record_feed(&b,wire+n-1,1,&used) == HDT_RECORD_ERROR);
    for (size_t i = 0; i < sizeof(b.plain); ++i) assert(b.plain[i] == 0);
    uint8_t ah[59];
    assert(hdt_record_init(&b,k2,k1,hash,0,ah));
    assert(hdt_record_feed(&b,oversized,3,&used) == HDT_RECORD_ERROR);
    assert(hdt_record_init(&a,k1,k2,hash,1,ah));
    ah[3] ^= 1;
    uint8_t bh[59];
    assert(hdt_record_init(&b,k2,k1,hash,0,bh));
    assert(hdt_record_feed(&b,ah,sizeof(ah),&used) == HDT_RECORD_ERROR);
    hdt_record_clear(&a); hdt_record_clear(&b);
    assert(!memcmp(&a,&b,sizeof(a)));
    printf("record state: %zu bytes (host ABI), RX pool+record: %zu bytes\n",
           sizeof(a),sizeof(a)+sizeof(hdt_udx_rx_t));
}
int main(void) {
    assert(sodium_init() >= 0);
    fragmented(); boundaries();
    puts("bounded encrypted framing, RX-pool drain, authentication and backpressure: PASS");
}
