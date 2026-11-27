/* SPDX-License-Identifier: LGPL-3.0-only
 * Bounded Noise responder and relay reply adapted from hyperdht-cpp.
 * See PROVENANCE.md. */
#include "hdtiny/listener.h"
#include "hdtiny/handshake.h"
#include "compact_internal.h"
#include "listener_punch.h"

int hdt_listener_init(hdt_listener *l, const uint8_t seed[32], const uint8_t key[32],
                       const hdt_addr *relays, size_t count) {
    if (!l || !seed || !key || count > HDT_PUBLISH_RELAYS || (count && !relays)) return 0;
    hdt_listener_clear(l);
    memcpy(l->seed, seed, sizeof(l->seed));
    if (count) memcpy(l->relays, relays, count * sizeof(*relays));
    l->relay_count = (uint8_t)count;
    crypto_generichash(l->target, sizeof(l->target), key, 32, NULL, 0);
    return 1;
}

static int private_ipv4(const hdt_addr *addr) {
    const unsigned a = addr->ip[0], b = addr->ip[1];
    return addr->port && (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168));
}

void hdt_listener_set_lan(hdt_listener *l, const hdt_addr *local, const uint8_t mask[4]) {
    if (!l || l->reply_size || !local || !mask || !private_ipv4(local) || sodium_is_zero(mask, 4))
        return;
    l->local = *local;
    memcpy(l->mask, mask, sizeof(l->mask));
}

static void select_lan(hdt_listener *l, const hdt_addr *external, const hdt_addr *peer,
                       const uint8_t *payload, size_t size) {
    if (!l->local.port || !external || !external->port || memcmp(external->ip, peer->ip, 4)) return;
    reader r = {payload, size, 1};
    (void)read_uint(&r); /* Version. The complete payload has been validated. */
    const uint64_t flags = read_uint(&r);
    (void)read_uint(&r); /* Error. */
    if (read_uint(&r) == 1 || !(flags & 2)) return; /* OPEN or no IPv4 addresses. */
    if (flags & 1) {
        (void)read_uint(&r);
        const uint64_t count = read_uint(&r);
        if (!r.ok || count > r.left / 12) return;
        (void)take(&r, (size_t)count * 12);
    }
    const uint64_t count = read_uint(&r);
    for (size_t i = 0; r.ok && i < count; ++i) {
        const hdt_addr addr = read_addr(&r);
        if (!r.ok || !private_ipv4(&addr) || hdt_addr_equal(&addr, &l->local)) continue;
        uint8_t distance = 0;
        for (size_t j = 0; j < sizeof(l->mask); ++j)
            distance |= (addr.ip[j] ^ l->local.ip[j]) & l->mask[j];
        if (!distance) { l->lan_peer = addr; return; }
    }
}

static int adopt(hdt_listener *l, const hdt_addr *client, hdt_slice message, const hdt_addr *external) {
    hdt_noise *noise = &l->phase.noise;
    hdt_peer_info info;
    uint8_t ephemeral[32], payload[HDT_NOISE_PAYLOAD_MAX], keys[128];
    uint8_t header[HDT_RECORD_HEADER_SIZE];
    size_t n;
    const uint32_t id = randombytes_random();
    randombytes_buf(ephemeral, sizeof(ephemeral));
    const int initialized = hdt_noise_init(noise, 0, l->seed, ephemeral, NULL);
    sodium_memzero(ephemeral, sizeof(ephemeral));
    if (!initialized ||
        !hdt_noise_receive(noise, message.data, message.size, payload, sizeof(payload), &n) ||
        !hdt_handshake_peer(payload, n, &info)) {
        hdt_listener_reset(l);
        return 0;
    }
    select_lan(l, external, client, payload, n);
    n = external ? hdt_listener_punch_payload(l, id, payload, sizeof(payload))
                : hdt_handshake_payload(id, 0, payload, sizeof(payload));
    if (!n || !hdt_noise_send(noise, payload, n, l->reply, sizeof(l->reply), &l->reply_size)) {
        hdt_listener_reset(l);
        return 0;
    }
    if (external) {
        l->punch_mode = 1;
        l->punch_id = id;
        l->peer_open = info.firewall == 1;
        hdt_listener_punch_keys(l, noise->hash);
    }
    memcpy(keys, noise->tx, 32);
    memcpy(keys + 32, noise->rx, 32);
    memcpy(keys + 64, noise->hash, 64);
    sodium_memzero(&l->phase, sizeof(l->phase));
    hdt_connector *stream = &l->phase.stream;
    hdt_transport *transport = &stream->state.stream.transport;
    hdt_transport_init(transport, client, id, info.udx_id, 0, info.udx_seq);
    const int initialized_record = hdt_record_init(&stream->state.stream.record,
        keys, keys + 32, keys + 64, 0, header);
    sodium_memzero(keys, sizeof(keys));
    if (!initialized_record || !hdt_transport_write(transport, header, sizeof(header))) {
        hdt_listener_reset(l);
        return 0;
    }
    l->client = *client;
    stream->status = HDT_CONNECT_HEADER;
    return 1;
}

static int selected_relay(const hdt_listener *l, const hdt_addr *from) {
    for (size_t i = 0; i < l->relay_count; ++i)
        if (hdt_addr_equal(from, &l->relays[i])) return 1;
    return 0;
}

static size_t lan_ping(hdt_listener *l, const hdt_addr *from, const hdt_rpc *request,
                       uint8_t *packet, size_t capacity) {
    if (!l->reply_size || l->peer_udx || l->punching ||
        !l->lan_peer.port || memcmp(l->lan_peer.ip, from->ip, 4) ||
        !hdt_addr_equal(&l->local, &request->observed))
        return 0;
    hdt_rpc reply = {0};
    reply.type = HDT_RPC_RESPONSE;
    reply.tid = request->tid;
    reply.observed = *from;
    const size_t count = hdt_rpc_encode(&reply, packet, capacity);
    if (count) {
        l->lan_ready = 1;
        // Stock JS can ping from a different socket than its advertised UDX port.
        l->phase.stream.state.stream.transport.peer = l->lan_peer;
    }
    return count;
}

size_t hdt_listener_handshake(hdt_listener *l, const hdt_addr *from, uint8_t *packet,
                              size_t size, size_t capacity) {
    hdt_rpc request, response = {0};
    hdt_slice message;
    hdt_addr client;
    int relayed = 0, has_peer;
    uint8_t fingerprint[32], value[sizeof(l->reply) + 16];
    if (!l || !from || !from->port || !packet || size > capacity ||
        !hdt_rpc_decode(packet, size, &request) || request.type != HDT_RPC_REQUEST) return 0;
    if (!request.command && (request.flags & HDT_INTERNAL) &&
        !(request.flags & ~(HDT_INTERNAL | HDT_HAS_ID)))
        return lan_ping(l, from, &request, packet, capacity);
    if (!request.target || !(request.flags & HDT_HAS_VALUE) ||
        sodium_memcmp(request.target, l->target, 32)) return 0;
    if (request.command == 1 && selected_relay(l, from))
        return hdt_listener_punch_reply(l, from, &request, packet, capacity);
    if (request.command) return 0;
    if (hdt_handshake_unwrap(request.value, 0, &message, &client, &has_peer)) {
        if (has_peer) return 0;
        client = *from;
    } else if (selected_relay(l, from) &&
               hdt_handshake_unwrap(request.value, 2, &message, &client, &has_peer) && has_peer) {
        relayed = 1;
    } else {
        return 0;
    }
    crypto_generichash(fingerprint, sizeof(fingerprint), message.data, message.size, NULL, 0);
    if (l->reply_size) {
        if (!hdt_addr_equal(&client, &l->client) ||
            sodium_memcmp(fingerprint, l->request_id, sizeof(fingerprint))) return 0;
    } else {
        // A delayed retry of the last finished handshake must not take the slot.
        if (!sodium_memcmp(fingerprint, l->request_id, sizeof(fingerprint)) ||
            !adopt(l, &client, message, relayed ? &request.observed : NULL)) return 0;
        memcpy(l->request_id, fingerprint, sizeof(fingerprint));
    }

    hdt_slice noise = {l->reply, l->reply_size};
    size_t value_size;
    if (relayed) {
        writer w = {value, sizeof(value), 1};
        write_uint(&w, 1); /* Client address follows the Noise reply. */
        write_uint(&w, 1); /* FROM_SERVER */
        write_uint(&w, noise.size);
        put(&w, noise.data, noise.size);
        write_addr(&w, client);
        if (!w.ok) return 0;
        value_size = sizeof(value) - w.left;
    } else {
        value_size = hdt_handshake_wrap(4, noise, value, sizeof(value));
    }
    if (!value_size) return 0;
    response.type = relayed ? HDT_RPC_REQUEST : HDT_RPC_RESPONSE;
    response.flags = HDT_HAS_VALUE;
    response.tid = request.tid;
    response.observed = *from;
    if (relayed) {
        response.flags |= HDT_HAS_TARGET;
        response.target = l->target;
    }
    response.value = (hdt_slice){value, value_size};
    l->relayed |= (uint8_t)relayed;
    return hdt_rpc_encode(&response, packet, capacity);
}

int hdt_listener_receive(hdt_listener *l, const hdt_addr *from, const uint8_t *packet, size_t size) {
    if (!l || !l->reply_size || (l->punch_mode && !l->peer_open && !l->probe_sent && !l->lan_ready) ||
        !hdt_connector_receive(&l->phase.stream, from, packet, size)) return 0;
    l->peer_udx = 1;
    return 1;
}

size_t hdt_listener_poll(hdt_listener *l, uint64_t now, hdt_addr *to, uint8_t *packet, size_t capacity) {
    if (!l || !l->reply_size) return 0;
    if (l->peer_udx || l->peer_open)
        return hdt_connector_poll(&l->phase.stream, now, to, packet, capacity);
    if (!l->punching || !capacity || !packet || !to ||
        (l->probe_sent && now - l->last_probe < 250)) return 0;
    packet[0] = 0;
    *to = l->phase.stream.state.stream.transport.peer;
    l->last_probe = now;
    l->probe_sent = 1;
    return 1;
}

void hdt_listener_reset(hdt_listener *l) {
    if (!l) return;
    sodium_memzero(&l->phase, sizeof(l->phase));
    sodium_memzero(l->reply, sizeof(l->reply));
    sodium_memzero(l->punch_secret, sizeof(l->punch_secret));
    sodium_memzero(l->token_secret, sizeof(l->token_secret));
    l->punch_round = l->punch_id = 0;
    l->reply_size = l->peer_udx = l->relayed = l->peer_open = 0;
    l->punching = l->probe_sent = l->punch_mode = l->lan_ready = 0;
    l->lan_peer.port = 0;
}

void hdt_listener_clear(hdt_listener *l) {
    if (l) sodium_memzero(l, sizeof(*l));
}
