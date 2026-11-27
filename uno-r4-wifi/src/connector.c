/* SPDX-License-Identifier: LGPL-3.0-only
 * Bounded adaptation of hyperdht-cpp connect.cpp/peer_connect.cpp sequencing. */
#include "hdtiny/connector.h"
#include "hdtiny/handshake.h"
#include <sodium.h>
#include <string.h>

void hdt_connector_clear(hdt_connector *c) {
    if (c) sodium_memzero(c, sizeof(*c));
}
static int fail(hdt_connector *c) { hdt_connector_clear(c); return 0; }
int hdt_connector_begin(hdt_connector *c, const hdt_addr *rendezvous,
                         const uint8_t selected[32], const uint8_t seed[32],
                         const uint8_t ephemeral[32], uint32_t id, uint16_t tid) {
    uint8_t plain[32], noise[HDT_NOISE_MESSAGE_MAX], value[624], target[32];
    size_t plain_size, noise_size, value_size;
    hdt_rpc request = {0};
    if (!c || !rendezvous || !rendezvous->port || !selected || !seed || !ephemeral) return 0;
    hdt_connector_clear(c);
    c->rendezvous = *rendezvous; c->local_id = id; c->tid = tid;
    if (!hdt_noise_init(&c->state.handshake.noise, 1, seed, ephemeral, selected)) return fail(c);
    plain_size = hdt_handshake_payload(id, 0, plain, sizeof(plain));
    if (!plain_size || !hdt_noise_send(&c->state.handshake.noise, plain, plain_size,
                                       noise, sizeof(noise), &noise_size)) return fail(c);
    hdt_slice slice = {noise, noise_size};
    value_size = hdt_handshake_wrap(0, slice, value, sizeof(value));
    crypto_generichash(target, sizeof(target), selected, 32, NULL, 0);
    request.type = HDT_RPC_REQUEST; request.flags = HDT_HAS_TARGET | HDT_HAS_VALUE;
    request.tid = tid; request.observed = *rendezvous;
    request.command = 0; request.target = target; request.value.data = value; request.value.size = value_size;
    c->state.handshake.size = hdt_rpc_encode(&request, c->state.handshake.request,
                                             sizeof(c->state.handshake.request));
    if (!value_size || !c->state.handshake.size) return fail(c);
    c->status = HDT_CONNECT_HANDSHAKE;
    return 1;
}
static void drain(hdt_connector *c) {
    hdt_transport *t = &c->state.stream.transport;
    hdt_record *r = &c->state.stream.record;
    const hdt_udx_rx_slot_t *slot;
    while (!r->pending && (slot = hdt_udx_rx_peek(&t->incoming))) {
        size_t used = 0;
        if (hdt_record_feed(r, slot->data + c->consumed, slot->length - c->consumed,
                             &used) == HDT_RECORD_ERROR) { fail(c); return; }
        c->consumed += (uint16_t)used;
        if (c->consumed == slot->length) {
            hdt_udx_rx_release(&t->incoming, slot->seq); c->consumed = 0;
            hdt_transport_consumed(t);
        }
    }
    if (t->remote_end == 2) {
        if (!r->header_received || r->body_used || r->prefix_used) { fail(c); return; }
        c->status = HDT_CONNECT_EOF;
    } else if (c->status == HDT_CONNECT_HEADER && r->header_received && !t->queued)
        c->status = HDT_CONNECT_READY;
}
size_t hdt_connector_poll(hdt_connector *c, uint64_t now, hdt_addr *destination,
                           uint8_t *packet, size_t capacity) {
    if (!c || !packet || !destination || c->status == HDT_CONNECT_FAILED) return 0;
    if (c->status == HDT_CONNECT_HANDSHAKE) {
        if (c->attempts && now - c->last_send < 1000) return 0;
        if (c->attempts == 3) { fail(c); return 0; }
        if (capacity < c->state.handshake.size) return 0;
        memcpy(packet, c->state.handshake.request, c->state.handshake.size);
        *destination = c->rendezvous; c->last_send = now; ++c->attempts;
        return c->state.handshake.size;
    }
    hdt_transport *t = &c->state.stream.transport;
    *destination = t->peer;
    size_t size = hdt_transport_poll(t, now, packet, capacity);
    if (t->closed) { fail(c); return 0; }
    return size;
}
int hdt_connector_receive(hdt_connector *c, const hdt_addr *source,
                           const uint8_t *packet, size_t size) {
    if (!c || !source || !packet || c->status == HDT_CONNECT_FAILED) return 0;
    if (c->status == HDT_CONNECT_HANDSHAKE) {
        hdt_rpc response;
        hdt_slice noise;
        hdt_addr peer;
        hdt_peer_info info;
        int has_peer;
        uint8_t plain[HDT_NOISE_PAYLOAD_MAX], keys[128], header[59];
        size_t written;
        if (!hdt_addr_equal(source, &c->rendezvous) || !hdt_rpc_decode(packet, size, &response) ||
            response.type != HDT_RPC_RESPONSE || response.tid != c->tid || response.error ||
            !(response.flags & HDT_HAS_VALUE) ||
            !hdt_handshake_unwrap(response.value, 4, &noise, &peer, &has_peer)) return 0;
        if (!hdt_noise_receive(&c->state.handshake.noise, noise.data, noise.size, plain,
                                sizeof(plain), &written)) return fail(c);
        if (!hdt_handshake_peer(plain, written, &info) || info.firewall != 1) return fail(c);
        memcpy(keys, c->state.handshake.noise.tx, 32);
        memcpy(keys + 32, c->state.handshake.noise.rx, 32);
        memcpy(keys + 64, c->state.handshake.noise.hash, 64);
        sodium_memzero(&c->state, sizeof(c->state));
        hdt_transport_init(&c->state.stream.transport, has_peer ? &peer : source,
                             c->local_id, info.udx_id, 0, info.udx_seq);
        int ok = hdt_record_init(&c->state.stream.record, keys, keys + 32, keys + 64, 1, header);
        sodium_memzero(keys, sizeof(keys));
        if (!ok || !hdt_transport_write(&c->state.stream.transport, header, sizeof(header))) return fail(c);
        c->status = HDT_CONNECT_HEADER;
        return 1;
    }
    if (!hdt_transport_receive(&c->state.stream.transport, source, packet, size)) return 0;
    if (c->state.stream.transport.closed) return fail(c);
    drain(c);
    return c->status != HDT_CONNECT_FAILED;
}
int hdt_connector_write(hdt_connector *c, const uint8_t *plain, size_t size) {
    if (!c || (c->status != HDT_CONNECT_READY && c->status != HDT_CONNECT_EOF) || c->state.stream.transport.queued ||
        c->state.stream.transport.local_end || !size) return 0;
    hdt_transport *t = &c->state.stream.transport;
    size_t written = hdt_record_seal(&c->state.stream.record, plain, size, t->tx, sizeof(t->tx));
    return written && hdt_transport_write(t, t->tx, written);
}
const uint8_t *hdt_connector_read(const hdt_connector *c, size_t *size) {
    if (size) *size = 0;
    if (!c || !size || (c->status != HDT_CONNECT_READY && c->status != HDT_CONNECT_EOF) ||
        !c->state.stream.record.pending) return NULL;
    *size = c->state.stream.record.plain_size;
    return c->state.stream.record.plain;
}
void hdt_connector_release(hdt_connector *c) {
    if (!c || c->status < HDT_CONNECT_HEADER) return;
    hdt_record_release(&c->state.stream.record); drain(c);
}
int hdt_connector_end(hdt_connector *c) {
    return c && (c->status == HDT_CONNECT_READY || c->status == HDT_CONNECT_EOF) &&
           hdt_transport_end(&c->state.stream.transport);
}
