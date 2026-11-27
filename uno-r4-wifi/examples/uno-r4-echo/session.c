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

uint8_t *hdt_r4_bench_packet(void) { return hdt_measure_state.packet; }

/* One bounded echo session, following platform/emulated-m4/guest.c. */
#include "session.h"
static uint8_t sent, echoed, ending, looking;
static hdt_addr observers[HDT_PUNCH_OBSERVERS];
static size_t observer_count;
static hdt_addr wifi;
static uint8_t subnet[4];

void hdt_r4_bench_set_lan(const hdt_addr *local, const uint8_t mask[4]) {
    wifi = *local;
    memcpy(subnet, mask, sizeof(subnet));
}

int hdt_r4_bench_lan_destination(const hdt_addr *to) {
    return !looking && hdt_connector_lan_destination(&hdt_measure_state.phase.connector, to);
}

int hdt_r4_bench_begin(const uint8_t key[32], const hdt_addr *seeds, size_t count) {
    struct measurement_state *s = &hdt_measure_state;
    if (!key || !seeds || !count) return 0;
    memcpy(s->selected_key, key, sizeof(s->selected_key));
    observer_count = count < HDT_PUNCH_OBSERVERS ? count : HDT_PUNCH_OBSERVERS;
    memcpy(observers, seeds, observer_count * sizeof(*seeds));
    looking = hdt_find_begin(&s->phase.discovery, key, seeds, count,
                             (uint16_t)randombytes_random(), 1000) != 0;
    return looking;
}

int hdt_r4_bench_looking(void) { return looking; }

static int connect_rendezvous(const hdt_addr *peer) {
    struct measurement_state *s = &hdt_measure_state;
    randombytes_buf(s->identity_seed, sizeof(s->identity_seed));
    randombytes_buf(s->ephemeral_seed, sizeof(s->ephemeral_seed));
    int ok = hdt_connector_begin_nat(&s->phase.connector, peer, s->selected_key,
                                 s->identity_seed, s->ephemeral_seed,
                                 randombytes_random(), (uint16_t)randombytes_random(), observers, observer_count, &wifi, subnet);
    sodium_memzero(s->identity_seed, sizeof(s->identity_seed));
    sodium_memzero(s->ephemeral_seed, sizeof(s->ephemeral_seed));
    return ok;
}

int hdt_r4_bench_step(uint64_t now, const hdt_addr *source, size_t received,
                       hdt_addr *destination, size_t *outgoing) {
    struct measurement_state *s = &hdt_measure_state;
    hdt_connector *c = &s->phase.connector;
    *outgoing = 0;
    if (looking) {
        hdt_discovery *d = &s->phase.discovery;
        if (received) (void)hdt_find_receive(d, source, s->packet, received);
        if (d->status == HDT_FIND_FOUND) {
            hdt_addr rendezvous = d->rendezvous;
            looking = 0;
            if (!connect_rendezvous(&rendezvous)) return -1;
            received = 0;
        } else {
            *outgoing = hdt_find_next(d, now, destination, s->packet, 1460);
            return d->status == HDT_FIND_RUNNING ? 0 : -2;
        }
    }
    if (received) (void)hdt_connector_receive(c, source, s->packet, received);
    if (c->status == HDT_CONNECT_FAILED) return -1;
    if (!sent && c->status == HDT_CONNECT_READY) {
        for (size_t i = 0; i < 1024; ++i) s->packet[i] = (uint8_t)i;
        sent = hdt_connector_write(c, s->packet, 1024) != 0;
    }
    size_t size;
    const uint8_t *plain = hdt_connector_read(c, &size);
    if (plain) {
        if (echoed || size != 1024) return -1;
        for (size_t i = 0; i < size; ++i)
            if (plain[i] != (uint8_t)i) return -1;
        echoed = 1;
        hdt_connector_release(c);
    }
    if (sent && echoed && !ending) ending = hdt_connector_end(c) != 0;
    *outgoing = hdt_connector_poll(c, now, destination, s->packet, 1460);
    if (c->status == HDT_CONNECT_FAILED) return -1;
    return !*outgoing && sent && echoed && ending && c->status == HDT_CONNECT_EOF &&
           c->state.stream.transport.local_end && !c->state.stream.transport.queued;
}

void hdt_r4_bench_clear(void) {
    sodium_memzero(&hdt_measure_state, sizeof(hdt_measure_state));
    sent = echoed = ending = looking = 0;
}
