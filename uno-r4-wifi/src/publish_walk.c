/* SPDX-License-Identifier: LGPL-3.0-only
 * Bounded FIND_PEER walk based on hyperdht-cpp query.cpp and the Tiny connector's ranked walk. */
#include "hdtiny/publish_walk.h"
#include "compact_internal.h"
#include <sodium.h>

enum { FIND_PEER = 2, LOOKUP_TIMEOUT = 700, UNKNOWN_COMMAND = 1 };

static void ranked_insert(hdt_publish_candidate *list, uint8_t *count, size_t cap,
                          const hdt_addr *addr, const uint8_t id[32], const uint8_t target[32]) {
    hdt_publish_candidate c;
    c.addr = *addr;
    for (size_t i = 0; i < sizeof(c.distance); ++i) c.distance[i] = id[i] ^ target[i];
    size_t pos = 0;
    while (pos < *count && memcmp(list[pos].distance, c.distance, sizeof(c.distance)) <= 0) ++pos;
    if (pos == cap) return;
    if (*count == cap) --*count;
    memmove(list + pos + 1, list + pos, (*count - pos) * sizeof(c));
    list[pos] = c;
    ++*count;
}

static void candidate(hdt_publish_walk *w, const hdt_addr *addr) {
    if (!addr->port) return;
    for (size_t i = 0; i < w->visited_count; ++i)
        if (hdt_addr_equal(addr, &w->visited[i])) return;
    for (size_t i = 0; i < w->pending_count; ++i)
        if (hdt_addr_equal(addr, &w->pending[i].addr)) return;
    uint8_t endpoint[6], id[32];
    writer out = {endpoint, sizeof(endpoint), 1};
    write_addr(&out, *addr);
    crypto_generichash(id, sizeof(id), endpoint, sizeof(endpoint), NULL, 0);
    ranked_insert(w->pending, &w->pending_count, HDT_PUBLISH_CANDIDATES, addr, id, w->target);
}

int hdt_publish_walk_begin(hdt_publish_walk *w, const uint8_t key[32], const hdt_addr *seeds, size_t count) {
    if (!w || !key || !seeds || !count || count > HDT_PUBLISH_CANDIDATES) return 0;
    memset(w, 0, sizeof(*w));
    crypto_generichash(w->target, sizeof(w->target), key, 32, NULL, 0);
    for (size_t i = 0; i < count; ++i) candidate(w, &seeds[i]);
    return w->pending_count != 0;
}

size_t hdt_publish_walk_poll(hdt_publish_walk *w, uint64_t now, hdt_addr *to,
                             uint8_t *packet, size_t capacity) {
    if (!w || !to || !packet || w->done) return 0;
    if (w->awaiting && now - w->sent < LOOKUP_TIMEOUT) return 0;
    if (!w->pending_count || w->visited_count == HDT_PUBLISH_VISITS) {
        w->awaiting = 0;
        w->done = 1;
        return 0;
    }
    hdt_rpc req = {0};
    req.type = HDT_RPC_REQUEST;
    req.flags = HDT_HAS_TARGET;
    req.command = FIND_PEER;
    req.tid = (uint16_t)randombytes_random();
    req.target = w->target;
    req.observed = w->pending[0].addr;
    const size_t count = hdt_rpc_encode(&req, packet, capacity);
    if (!count) return 0;
    *to = req.observed;
    w->visited[w->visited_count++] = *to;
    memmove(w->pending, w->pending + 1, (--w->pending_count) * sizeof(w->pending[0]));
    w->tid = req.tid;
    w->sent = now;
    w->awaiting = 1;
    return count;
}

int hdt_publish_walk_receive(hdt_publish_walk *w, const hdt_addr *from,
                               const uint8_t *packet, size_t size, int (*allow)(const hdt_addr *)) {
    hdt_rpc reply;
    if (!w || !w->awaiting || !allow || !hdt_addr_equal(from, &w->visited[w->visited_count - 1]) ||
        !hdt_rpc_decode(packet, size, &reply) || reply.type != HDT_RPC_RESPONSE || reply.tid != w->tid)
        return 0;
    if (!reply.error && reply.id && reply.token)
        ranked_insert(w->best, &w->best_count, HDT_PUBLISH_RELAYS, from, reply.id, w->target);
    // An ephemeral bootstrap can reject FIND_PEER and still supply closer nodes.
    if (!reply.error || reply.error == UNKNOWN_COMMAND) {
        for (size_t i = 0; i < reply.closer.size / 6; ++i) {
            hdt_addr addr;
            if (hdt_rpc_closer(&reply, i, &addr) && allow(&addr)) candidate(w, &addr);
        }
    }
    w->awaiting = 0;
    return 1;
}
