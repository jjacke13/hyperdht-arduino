/* SPDX-License-Identifier: LGPL-3.0-only
 * Bounded self-publication adapted from hyperdht-cpp, see PROVENANCE.md. */
#include "hdtiny/publish.h"

#include <sodium.h>
#include <string.h>

enum { FIND_PEER = 2, MAX_ATTEMPTS = 3, TIMEOUT_MS = 1000 };

static void finish(hdt_publish *p, hdt_publish_status status) {
    p->awaiting = 0;
    p->status = status;
    sodium_memzero(p->token, sizeof(p->token));
}

int hdt_publish_begin(hdt_publish *p, const hdt_addr *node, const uint8_t key[32],
                      hdt_publish_action action) {
    if (!p || !node || !node->port || !key ||
        (action != HDT_PUBLISH_ANNOUNCE && action != HDT_PUBLISH_WITHDRAW)) return 0;
    memset(p, 0, sizeof(*p));
    p->node = *node;
    p->action = action;
    p->status = HDT_PUBLISH_TOKEN;
    memcpy(p->key, key, sizeof(p->key));
    crypto_generichash(p->target, sizeof(p->target), key, 32, NULL, 0);
    return 1;
}

static int sign_value(const hdt_publish *p, const uint8_t secret_key[64], uint8_t value[98]) {
    uint8_t ns_input[33], signable[64], preimage[32 + 32 + 32 + 33];
    static const uint8_t ns[] = "hyperswarm/dht";
    crypto_generichash(ns_input, 32, ns, sizeof(ns) - 1, NULL, 0);
    ns_input[32] = (uint8_t)p->action;
    crypto_generichash(signable, 32, ns_input, sizeof(ns_input), NULL, 0);

    value[0] = 5; /* Peer record and signature. */
    memcpy(value + 1, p->key, 32);
    value[33] = 0; /* No extra relay addresses. The node stores our UDP source. */
    memcpy(preimage, p->target, 32);
    memcpy(preimage + 32, p->node_id, 32);
    memcpy(preimage + 64, p->token, 32);
    memcpy(preimage + 96, value + 1, 33);
    crypto_generichash(signable + 32, 32, preimage, sizeof(preimage), NULL, 0);
    sodium_memzero(preimage, sizeof(preimage));
    return crypto_sign_detached(value + 34, NULL, signable, sizeof(signable), secret_key) == 0;
}

size_t hdt_publish_poll(hdt_publish *p, uint64_t now, const uint8_t secret_key[64],
                        uint8_t *packet, size_t capacity) {
    if (!p || !packet || (p->status != HDT_PUBLISH_TOKEN && p->status != HDT_PUBLISH_COMMIT))
        return 0;
    if (p->awaiting) {
        if (now - p->sent < TIMEOUT_MS) return 0;
        if (p->attempts == MAX_ATTEMPTS) {
            finish(p, HDT_PUBLISH_FAILED);
            return 0;
        }
        finish(p, HDT_PUBLISH_TOKEN);
    }
    hdt_rpc req = {0};
    uint8_t value[98], signing_key[32];
    const int commit = p->status == HDT_PUBLISH_COMMIT;
    req.type = HDT_RPC_REQUEST;
    req.flags = HDT_HAS_TARGET;
    req.command = FIND_PEER;
    req.target = p->target;
    req.observed = p->node;
    req.tid = (uint16_t)randombytes_random();
    if (commit) {
        if (!secret_key || crypto_sign_ed25519_sk_to_pk(signing_key, secret_key) ||
            sodium_memcmp(signing_key, p->key, sizeof(signing_key)) ||
            !sign_value(p, secret_key, value)) {
            finish(p, HDT_PUBLISH_FAILED);
            return 0;
        }
        req.flags |= HDT_HAS_TOKEN | HDT_HAS_VALUE;
        req.command = (uint32_t)p->action;
        req.token = p->token;
        req.value = (hdt_slice){value, sizeof(value)};
    }
    const size_t count = hdt_rpc_encode(&req, packet, capacity);
    if (count) {
        p->tid = req.tid;
        p->sent = now;
        p->awaiting = 1;
        if (!commit) ++p->attempts;
    }
    return count;
}

int hdt_publish_receive(hdt_publish *p, const hdt_addr *from, const uint8_t *packet, size_t size) {
    hdt_rpc reply;
    if (!p || !p->awaiting || !hdt_addr_equal(&p->node, from) ||
        !hdt_rpc_decode(packet, size, &reply) || reply.type != HDT_RPC_RESPONSE ||
        reply.tid != p->tid || reply.error) return 0;
    if (p->status == HDT_PUBLISH_TOKEN) {
        if (!reply.id || !reply.token) return 0;
        memcpy(p->node_id, reply.id, sizeof(p->node_id));
        memcpy(p->token, reply.token, sizeof(p->token));
        p->status = HDT_PUBLISH_COMMIT;
        p->awaiting = 0;
        return 1;
    }
    if (p->status != HDT_PUBLISH_COMMIT || (reply.flags & (HDT_HAS_TOKEN | HDT_HAS_VALUE)) ||
        (reply.id && sodium_memcmp(reply.id, p->node_id, sizeof(p->node_id)))) return 0;
    p->observed = reply.observed;
    finish(p, HDT_PUBLISH_DONE);
    return 1;
}
