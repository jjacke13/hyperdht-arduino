/* SPDX-License-Identifier: LGPL-3.0-only */
#include "hdtiny/discovery.h"
#include <assert.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

static const hdt_addr seed = {{192, 0, 2, 1}, 49737};
static const hdt_addr other = {{192, 0, 2, 2}, 49737};
static uint8_t key[32];

static void codecs(void) {
    uint8_t buf[256], small[256], id[32] = {1}, token[32] = {2}, value[4] = {0,1,2,3};
    uint8_t closer[] = {192,0,2,2,0x49,0xc2};
    hdt_rpc m = {0}, out;
    m.type = HDT_RPC_REQUEST; m.flags = 31; m.tid = 0xffff; m.observed = seed;
    m.id = id; m.token = token; m.target = key; m.command = UINT32_MAX;
    m.value.data = value; m.value.size = sizeof(value);
    for (int response = 0; response < 2; ++response) {
        if (response) {
            m.type = HDT_RPC_RESPONSE; m.error = UINT32_MAX;
            m.closer.data = closer; m.closer.size = sizeof(closer);
        }
        size_t n = hdt_rpc_encode(&m, buf, sizeof(buf));
        assert(n && hdt_rpc_decode(buf, n, &out));
        assert(out.tid == m.tid && out.flags == 31 && hdt_addr_equal(&out.observed, &seed));
        assert(out.value.size == 4 && !memcmp(out.value.data, value, 4));
        assert(response ? out.error == UINT32_MAX : out.command == UINT32_MAX);
        for (size_t i = 0; i < n; ++i) {
            assert(!hdt_rpc_decode(buf, i, &out));
            assert(out.type == 0);
            memset(small, 0xa5, sizeof(small));
            assert(!hdt_rpc_encode(&m, small, i));
            for (size_t j = i; j < sizeof(small); ++j) assert(small[j] == 0xa5);
        }
        buf[n] = 0;
        assert(!hdt_rpc_decode(buf, n + 1, &out));
    }
    uint8_t bad[] = {0x13, 0x10, 1,0, 192,0,2,1, 1,0,
                     0xff, 255,255,255,255,255,255,255,255};
    assert(!hdt_rpc_decode(bad, sizeof(bad), &out));
    bad[1] = 4; /* overflowing closer count */
    assert(!hdt_rpc_decode(bad, sizeof(bad), &out));
    bad[1] = 32; assert(!hdt_rpc_decode(bad, sizeof(bad), &out));
    assert(!hdt_rpc_decode(NULL, 0, &out));
    assert(!hdt_rpc_decode(bad, sizeof(bad), NULL));
    assert(!hdt_rpc_encode(NULL, buf, sizeof(buf)));
}
static void lookup(void) {
    hdt_discovery d;
    hdt_addr dest, relays[4];
    hdt_rpc req, resp = {0};
    uint8_t packet[256], answer[256], record[39], closer[6] = {192,0,2,2,0x49,0xc2};
    assert(hdt_find_begin(&d, key, &seed, 1, 65535, 100));
    uint8_t hash[32];
    assert(!crypto_generichash(hash, 32, key, 32, NULL, 0));
    assert(!memcmp(d.target, hash, 32));
    assert(!hdt_find_next(&d, 0, &dest, packet, 42));
    assert(!d.awaiting && !d.visited_count);
    size_t n = hdt_find_next(&d, 0, &dest, packet, sizeof(packet));
    assert(n == 43 && hdt_addr_equal(&dest, &seed));
    assert(hdt_rpc_decode(packet, n, &req));
    assert(req.command == 2 && req.flags == HDT_HAS_TARGET && req.tid == 1);
    assert(!memcmp(req.target, hash, 32));
    assert(!hdt_find_next(&d, 99, &dest, packet, sizeof(packet)));
    resp.type = HDT_RPC_RESPONSE; resp.tid = req.tid; resp.flags = HDT_HAS_CLOSER;
    resp.observed = other; /* wire address describes us, not the actual sender */
    resp.closer.data = closer; resp.closer.size = 6;
    n = hdt_rpc_encode(&resp, answer, sizeof(answer));
    assert(!hdt_find_receive(&d, &other, answer, n));
    resp.tid++; n = hdt_rpc_encode(&resp, answer, sizeof(answer));
    assert(!hdt_find_receive(&d, &seed, answer, n));
    resp.tid--; n = hdt_rpc_encode(&resp, answer, sizeof(answer));
    assert(hdt_find_receive(&d, &seed, answer, n));
    assert(d.candidates_count == 1);
    assert(hdt_find_next(&d, 100, &dest, packet, sizeof(packet)) == 43);
    assert(hdt_addr_equal(&dest, &other));
    memcpy(record, key, 32); record[32] = 1; memcpy(record + 33, closer, 6);
    resp.flags = HDT_HAS_VALUE; resp.value.data = record; resp.value.size = sizeof(record);
    resp.tid = d.tid;
    n = hdt_rpc_encode(&resp, answer, sizeof(answer));
    assert(hdt_find_receive(&d, &other, answer, n));
    assert(d.status == HDT_FIND_FOUND && d.relay_count == 1);
    assert(hdt_addr_equal(&d.rendezvous, &other) && hdt_addr_equal(&d.relays[0], &other));
    assert(!hdt_find_next(&d, 999, &dest, packet, sizeof(packet)));
    size_t count;
    hdt_slice r = {record, sizeof(record)};
    record[0] ^= 1;
    assert(!hdt_peer_record(r, key, relays, 4, &count) && count == 0);
    record[0] ^= 1;
    for (size_t i = 0; i < sizeof(record); ++i) {
        r.size = i;
        assert(!hdt_peer_record(r, key, relays, 4, &count));
    }
    r.size = sizeof(record);
    assert(!hdt_peer_record(r, key, relays, 0, &count));
    record[37] = record[38] = 0;
    assert(!hdt_peer_record(r, key, relays, 4, &count));
}
static void bounds(void) {
    hdt_discovery d;
    hdt_addr dest, seeds[9];
    uint8_t packet[256], old[256];
    hdt_rpc resp = {0};
    assert(!hdt_find_begin(&d, NULL, &seed, 1, 0, 100));
    assert(d.status == HDT_FIND_IDLE);
    assert(!hdt_find_begin(&d, key, &seed, 1, 0, 0));
    assert(hdt_find_begin(&d, key, &seed, 1, 0, 100));
    size_t n = hdt_find_next(&d, 0, &dest, packet, sizeof(packet));
    memcpy(old, packet, n);
    for (uint64_t t = 100; t <= 200; t += 100) {
        assert(hdt_find_next(&d, t, &dest, packet, sizeof(packet)) == n);
        assert(!memcmp(old, packet, n));
    }
    assert(!hdt_find_next(&d, 300, &dest, packet, sizeof(packet)));
    assert(d.status == HDT_FIND_EXHAUSTED);
    for (size_t i = 0; i < 9; ++i) { seeds[i] = seed; seeds[i].port += (uint16_t)i; }
    assert(hdt_find_begin(&d, key, seeds, 9, 0, 1));
    resp.type = HDT_RPC_RESPONSE; resp.observed = seed;
    for (size_t i = 0; i < 8; ++i) {
        assert(hdt_find_next(&d, i, &dest, packet, sizeof(packet)));
        resp.tid = d.tid; n = hdt_rpc_encode(&resp, old, sizeof(old));
        assert(hdt_find_receive(&d, &dest, old, n));
    }
    assert(!hdt_find_next(&d, 9, &dest, packet, sizeof(packet)));
    assert(d.status == HDT_FIND_LIMIT);
    /* A hostile chain cannot make the visited list or total query count grow. */
    assert(hdt_find_begin(&d, key, &seed, 1, 0, 1));
    uint8_t closer[6] = {192,0,2,3,0x49,0xc2};
    resp.flags = HDT_HAS_CLOSER; resp.closer.data = closer; resp.closer.size = 6;
    for (size_t i = 0; i < HDT_FIND_VISITED; ++i) {
        assert(hdt_find_next(&d, i, &dest, packet, sizeof(packet)));
        closer[3]++; resp.tid = d.tid;
        n = hdt_rpc_encode(&resp, old, sizeof(old));
        assert(hdt_find_receive(&d, &dest, old, n));
    }
    assert(!hdt_find_next(&d, 100, &dest, packet, sizeof(packet)));
    assert(d.status == HDT_FIND_LIMIT && d.visited_count == HDT_FIND_VISITED);
    printf("discovery workspace: %zu bytes (host ABI)\n", sizeof(d));
}
int main(void) {
    assert(sodium_init() >= 0);
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)i;
    codecs(); lookup(); bounds();
    puts("RPC truncation/bounds, transaction/source matching, walk/retry limits: PASS");
}
