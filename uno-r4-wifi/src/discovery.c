/* SPDX-License-Identifier: LGPL-3.0-only
 * Bounded adaptation of hyperdht-cpp dht_ops.cpp/query.cpp/rpc.cpp; provenance below. */
#include "hdtiny/discovery.h"
#include <sodium.h>
#include <string.h>

static void candidate(hdt_discovery *d, hdt_addr a) {
    if (!a.port) return;
    for (size_t i = 0; i < d->visited_count; ++i)
        if (hdt_addr_equal(&a, &d->visited[i])) return;
    for (size_t i = 0; i < d->candidates_count; ++i)
        if (hdt_addr_equal(&a, &d->candidates[i])) return;
    if (d->candidates_count == HDT_FIND_CANDIDATES) { d->limited = 1; return; }
    d->candidates[d->candidates_count++] = a;
}
int hdt_find_begin(hdt_discovery *d, const uint8_t public_key[32],
                   const hdt_addr *seeds, size_t count,
                   uint16_t initial_tid, uint32_t timeout_ms) {
    if (!d) return 0;
    memset(d, 0, sizeof(*d));
    if (!public_key || !seeds || !count || !timeout_ms || timeout_ms > 60000) return 0;
    memcpy(d->public_key, public_key, 32);
    if (crypto_generichash(d->target, 32, public_key, 32, NULL, 0)) return 0;
    d->tid = initial_tid; d->timeout_ms = timeout_ms;
    /* Scanning caller input is bounded too; excess seeds mark an incomplete walk. */
    if (count > HDT_FIND_CANDIDATES) { count = HDT_FIND_CANDIDATES; d->limited = 1; }
    for (size_t i = 0; i < count; ++i) candidate(d, seeds[i]);
    if (!d->candidates_count) return 0;
    d->status = HDT_FIND_RUNNING;
    return 1;
}
size_t hdt_find_next(hdt_discovery *d, uint64_t now_ms, hdt_addr *destination,
                     uint8_t *packet, size_t capacity) {
    hdt_rpc request = {0};
    if (!d || !destination || !packet || capacity < 43 || d->status != HDT_FIND_RUNNING) return 0;
    if (d->awaiting && now_ms - d->sent_at < d->timeout_ms) return 0;
    if (d->awaiting && d->attempts == HDT_FIND_ATTEMPTS) d->awaiting = 0;
    if (!d->awaiting) {
        if (!d->candidates_count || d->visited_count == HDT_FIND_VISITED) {
            d->status = d->limited || d->candidates_count ? HDT_FIND_LIMIT : HDT_FIND_EXHAUSTED;
            return 0;
        }
        hdt_addr a = d->candidates[0];
        memmove(d->candidates, d->candidates + 1, (--d->candidates_count) * sizeof(a));
        d->visited[d->visited_count++] = a;
        d->attempts = 0; d->awaiting = 1;
        if (++d->tid == 0) ++d->tid;
    }
    request.type = HDT_RPC_REQUEST; request.flags = HDT_HAS_TARGET;
    request.command = 2; request.target = d->target; request.tid = d->tid;
    request.observed = d->visited[d->visited_count - 1];
    size_t n = hdt_rpc_encode(&request, packet, capacity);
    if (!n) return 0;
    *destination = request.observed;
    d->sent_at = now_ms; ++d->attempts;
    return n;
}
int hdt_find_receive(hdt_discovery *d, const hdt_addr *source,
                      const uint8_t *packet, size_t size) {
    hdt_rpc response;
    if (!d || !d->awaiting || d->status != HDT_FIND_RUNNING ||
        !hdt_addr_equal(source, &d->visited[d->visited_count - 1]) ||
        !hdt_rpc_decode(packet, size, &response) || response.type != HDT_RPC_RESPONSE ||
        response.tid != d->tid) return 0;
    /* Wire 'observed' describes us; the UDP source identifies the rendezvous. */
    if (!(response.flags & HDT_HAS_ERROR) || !response.error) {
        if ((response.flags & HDT_HAS_VALUE) &&
            hdt_peer_record(response.value, d->public_key, d->relays,
                              HDT_FIND_RELAYS, &d->relay_count)) {
            d->rendezvous = *source; d->awaiting = 0; d->status = HDT_FIND_FOUND;
            return 1;
        }
        for (size_t i = 0; i < response.closer.size / 6; ++i) {
            hdt_addr a;
            if (hdt_rpc_closer(&response, i, &a)) candidate(d, a);
        }
    }
    d->awaiting = 0;
    return 1;
}
