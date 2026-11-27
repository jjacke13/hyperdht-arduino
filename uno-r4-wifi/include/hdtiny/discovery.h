/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_DISCOVERY_H
#define HDTINY_DISCOVERY_H
#include "rpc.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { HDT_FIND_CANDIDATES = 8, HDT_FIND_VISITED = 16, HDT_FIND_RELAYS = 4,
       HDT_FIND_ATTEMPTS = 3 };
typedef enum { HDT_FIND_IDLE, HDT_FIND_RUNNING, HDT_FIND_FOUND,
               HDT_FIND_EXHAUSTED, HDT_FIND_LIMIT } hdt_find_status;
typedef struct {
    uint8_t public_key[32], target[32];
    hdt_addr candidates[HDT_FIND_CANDIDATES], visited[HDT_FIND_VISITED];
    hdt_addr relays[HDT_FIND_RELAYS], rendezvous;
    size_t candidates_count, visited_count, relay_count;
    uint64_t sent_at;
    uint32_t timeout_ms;
    uint16_t tid;
    uint8_t attempts, awaiting, limited;
    hdt_find_status status;
} hdt_discovery;
/* Fresh workspace. Seeds are caller supplied, no DNS or network side effects.
 * initial_tid should come from the platform RNG. No identity allowlist. */
int hdt_find_begin(hdt_discovery *d, const uint8_t public_key[32],
                   const hdt_addr *seeds, size_t count,
                   uint16_t initial_tid, uint32_t timeout_ms);
/* Poll using monotonic milliseconds. Returns an encoded FIND_PEER datagram,
 * or zero while waiting/finished. Caller sends using the same UDP socket.
 * Calling next commits that attempt; a send failure is retried at the timeout. */
size_t hdt_find_next(hdt_discovery *d, uint64_t now_ms, hdt_addr *destination,
                     uint8_t *packet, size_t capacity);
/* Only matching actual UDP source + TID responses can advance state.
 * Returns one if the response was accepted. Peer records are not authentication. */
int hdt_find_receive(hdt_discovery *d, const hdt_addr *source,
                      const uint8_t *packet, size_t size);
#ifdef __cplusplus
}
#endif
#endif
