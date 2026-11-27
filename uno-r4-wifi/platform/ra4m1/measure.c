/* SPDX-License-Identifier: LGPL-3.0-only
 * Connector link harness + native begin/retry/clear check, NOT board firmware. */
#include "hdtiny/discovery.h"
#include "hdtiny/connector.h"
#include <string.h>

struct measurement_state {
    /* Discovery is discarded before connector begin; the connector itself
     * overlays Noise/request with transport/record. No second RX/TX fixture. */
    union { hdt_discovery discovery; hdt_connector connector; } phase;
    uint8_t packet[HDT_RPC_MAX_DATAGRAM];
    uint8_t selected_key[32], identity_seed[32], ephemeral_seed[32];
};
struct measurement_state hdt_measure_state;
volatile int hdt_measure_result;

static int self_check(void) {
    struct measurement_state *s = &hdt_measure_state;
    hdt_addr seed_addr = {{127, 0, 0, 1}, 49737}, destination;
    uint8_t secret[64];
    hdt_rpc request;
    if (sodium_init() < 0) return 1;
    randombytes_buf(s->identity_seed, sizeof(s->identity_seed));
    randombytes_buf(s->ephemeral_seed, sizeof(s->ephemeral_seed));
    /* A fresh test identity is used as a selected target; no packets are sent. */
    if (crypto_sign_seed_keypair(s->selected_key, secret, s->identity_seed)) return 2;
    sodium_memzero(secret, sizeof(secret));
    if (!hdt_find_begin(&s->phase.discovery, s->selected_key, &seed_addr, 1, 1, 1000) ||
        !hdt_find_next(&s->phase.discovery, 1, &destination, s->packet, sizeof(s->packet))) return 3;
    /* Exercise actual phased ownership with an explicit direct endpoint.
     * Host UDP discovery/full-session coverage lives in the reference suite. */
    hdt_connector *c = &s->phase.connector;
    if (!hdt_connector_begin(c, &seed_addr, s->selected_key, s->identity_seed,
                             s->ephemeral_seed, 17, 19)) return 4;
    if (hdt_connector_poll(c, 0, &destination, s->packet, 1) || c->attempts) return 5;
    size_t n = hdt_connector_poll(c, 0, &destination, s->packet, sizeof(s->packet));
    if (!n || !hdt_addr_equal(&destination, &seed_addr) ||
        !hdt_rpc_decode(s->packet, n, &request) || request.type != HDT_RPC_REQUEST ||
        request.tid != 19 || request.command != 0 || !(request.flags & HDT_HAS_VALUE)) return 6;
    if (hdt_connector_poll(c, 999, &destination, s->packet, sizeof(s->packet)) ||
        hdt_connector_poll(c, 1000, &destination, s->packet, sizeof(s->packet)) != n ||
        hdt_connector_poll(c, 2000, &destination, s->packet, sizeof(s->packet)) != n ||
        hdt_connector_poll(c, 3000, &destination, s->packet, sizeof(s->packet)) ||
        c->status != HDT_CONNECT_FAILED || !sodium_is_zero((const uint8_t *)c, sizeof(*c))) return 7;
    sodium_memzero(s, sizeof(*s));
    return 0;
}
int main(void) {
#ifndef HDT_MEASURE_HOST_TEST
    extern void hdt_ra4m1_install_entropy(void);
    hdt_ra4m1_install_entropy();
#endif
    hdt_measure_result = self_check();
    return hdt_measure_result;
}
