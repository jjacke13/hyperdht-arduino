/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_PUBLISH_WALK_H
#define HDTINY_PUBLISH_WALK_H
#include "rpc.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { HDT_PUBLISH_RELAYS = 3, HDT_PUBLISH_CANDIDATES = 8, HDT_PUBLISH_VISITS = 24 };
typedef struct { hdt_addr addr; uint8_t distance[32]; } hdt_publish_candidate;
typedef struct {
    uint8_t target[32];
    hdt_publish_candidate pending[HDT_PUBLISH_CANDIDATES], best[HDT_PUBLISH_RELAYS];
    hdt_addr visited[HDT_PUBLISH_VISITS];
    uint64_t sent;
    uint16_t tid;
    uint8_t pending_count, best_count, visited_count, awaiting, done;
} hdt_publish_walk;

/* Temporary ranked FIND_PEER workspace. Seed addresses must already be trusted.
 * Retain the best token-bearing replies, even for a key with no announcement. */
int hdt_publish_walk_begin(hdt_publish_walk *w, const uint8_t key[32], const hdt_addr *seeds, size_t count);
size_t hdt_publish_walk_poll(hdt_publish_walk *w, uint64_t now, hdt_addr *to,
                             uint8_t *packet, size_t capacity);
/* Reject untrusted address hints using the caller's deployment policy. */
int hdt_publish_walk_receive(hdt_publish_walk *w, const hdt_addr *from,
                               const uint8_t *packet, size_t size, int (*allow)(const hdt_addr *));

#ifdef __cplusplus
}
#endif
#endif
