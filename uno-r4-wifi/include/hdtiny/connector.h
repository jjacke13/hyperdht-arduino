/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_CONNECTOR_H
#define HDTINY_CONNECTOR_H
#include "noise.h"
#include "record.h"
#include "transport.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { HDT_CONNECT_FAILED, HDT_CONNECT_HANDSHAKE, HDT_CONNECT_HEADER,
               HDT_CONNECT_READY, HDT_CONNECT_EOF } hdt_connect_status;
typedef struct {
    union {
        struct { hdt_noise noise; uint8_t request[736]; size_t size; } handshake;
        struct { hdt_transport transport; hdt_record record; } stream;
    } state;
    hdt_addr rendezvous;
    uint64_t last_send;
    uint32_t local_id;
    uint16_t tid, consumed;
    uint8_t attempts;
    hdt_connect_status status;
} hdt_connector;
/* Direct, reachable IPv4 profile on one caller-owned UDP socket. Supply a
 * discovered rendezvous (or an explicit direct listener address), selected
 * identity, local persistent seed, fresh CSPRNG ephemeral seed, random stream
 * ID and transaction ID. No public network/side effects occur in this core.
 * Init fresh/cleared state only. Caller must have configured sodium/platform RNG.
 * This profile advertises OPEN: caller must actually have a reachable endpoint.
 * No overlapping inputs/outputs/state; calls serialized without reentrancy. */
int hdt_connector_begin(hdt_connector *c, const hdt_addr *rendezvous,
                         const uint8_t selected_key[32], const uint8_t seed[32],
                         const uint8_t ephemeral_seed[32], uint32_t id, uint16_t tid);
size_t hdt_connector_poll(hdt_connector *c, uint64_t now_ms, hdt_addr *destination,
                           uint8_t *packet, size_t capacity);
int hdt_connector_receive(hdt_connector *c, const hdt_addr *source,
                           const uint8_t *packet, size_t size);
/* One 1KiB plaintext record per write; returns zero on backpressure/closed.
 * Read borrows authenticated plaintext until release or terminal error/clear.
 * Releasing also drains any retained raw bytes and schedules window updates. */
int hdt_connector_write(hdt_connector *c, const uint8_t *plain, size_t size);
const uint8_t *hdt_connector_read(const hdt_connector *c, size_t *size);
void hdt_connector_release(hdt_connector *c);
int hdt_connector_end(hdt_connector *c);
void hdt_connector_clear(hdt_connector *c);
#ifdef __cplusplus
}
#endif
#endif
