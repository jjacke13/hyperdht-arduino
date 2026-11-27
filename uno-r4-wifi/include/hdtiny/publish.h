/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_PUBLISH_H
#define HDTINY_PUBLISH_H
#include "rpc.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { HDT_PUBLISH_ANNOUNCE = 4, HDT_PUBLISH_WITHDRAW = 5 } hdt_publish_action;
typedef enum { HDT_PUBLISH_IDLE, HDT_PUBLISH_TOKEN, HDT_PUBLISH_COMMIT,
               HDT_PUBLISH_DONE, HDT_PUBLISH_FAILED } hdt_publish_status;
typedef struct {
    hdt_addr node, observed;
    uint8_t key[32], target[32], node_id[32], token[32];
    uint64_t sent;
    uint16_t tid;
    uint8_t attempts, awaiting;
    hdt_publish_action action;
    hdt_publish_status status;
} hdt_publish;

/* One transaction at one caller-selected external node. Re-run ANNOUNCE with
 * the same identity to refresh. Initialize sodium/platform randomness first.
 * Use the listener's UDP socket for every send. */
int hdt_publish_begin(hdt_publish *p, const hdt_addr *node, const uint8_t key[32],
                      hdt_publish_action action);
/* secret_key is the persistent Ed25519 signing key and is never retained.
 * Only successful encodes
 * commit a send. Retry fetches a fresh token, including after a lost commit ACK. */
size_t hdt_publish_poll(hdt_publish *p, uint64_t now_ms, const uint8_t secret_key[64],
                        uint8_t *packet, size_t capacity);
int hdt_publish_receive(hdt_publish *p, const hdt_addr *from, const uint8_t *packet, size_t size);

#ifdef __cplusplus
}
#endif
#endif
