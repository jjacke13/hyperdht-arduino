/* SPDX-License-Identifier: LGPL-3.0-only
 * Bounded stable-mapping responder adapted from HyperDHT server/holepunch.
 * See PROVENANCE.md. */
#include "listener_punch.h"
#include "compact_internal.h"

enum {
    FROM_SERVER = 1, FROM_RELAY = 2,
    PUNCHING = 2, ADDRESSES = 4, REMOTE_ADDRESS = 8, TOKEN = 16, REMOTE_TOKEN = 32,
    OPEN = 1, CONSISTENT = 2, ABORTED = 1, COMMAND = 1,
    NONCE = crypto_secretbox_NONCEBYTES, OVERHEAD = NONCE + crypto_secretbox_MACBYTES
};

void hdt_listener_observe(hdt_listener *l, const hdt_addr *relay, const hdt_addr *observed) {
    if (!l || l->reply_size || !observed) return;
    for (size_t i = 0; i < l->relay_count; ++i)
        if (hdt_addr_equal(relay, &l->relays[i])) l->observed[i] = *observed;
}

static const hdt_addr *stable_mapping(const hdt_listener *l) {
    const hdt_addr *match = NULL;
    size_t count = 0;
    for (size_t i = 0; i < l->relay_count; ++i) {
        const hdt_addr *candidate = &l->observed[i];
        if (!candidate->port) continue;
        if (match && !hdt_addr_equal(match, candidate)) return NULL;
        match = candidate;
        ++count;
    }
    return count >= 2 ? match : NULL;
}

size_t hdt_listener_punch_payload(hdt_listener *l, uint32_t id, uint8_t *out, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < l->relay_count; ++i) count += l->observed[i].port != 0;
    if (!count) return 0;
    writer w = {out, capacity, 1};
    write_uint(&w, 1);  /* Noise payload v1 */
    write_uint(&w, 25 | (l->lan_peer.port ? 2 : 0)); /* Optional IPv4, plus holepunch, UDX and SecretStream. */
    write_uint(&w, 0);  /* error */
    write_uint(&w, 0);  /* unknown until sampling establishes a mapping */
    write_uint(&w, id);
    write_uint(&w, count);
    for (size_t i = 0; i < l->relay_count; ++i) {
        if (!l->observed[i].port) continue;
        write_addr(&w, l->relays[i]);
        write_addr(&w, l->observed[i]);
    }
    if (l->lan_peer.port) {
        write_uint(&w, 1);
        write_addr(&w, l->local);
    }
    write_uint(&w, 1); write_uint(&w, 1); /* UDX v1, reusable socket */
    write_uint(&w, id); write_uint(&w, 0);
    write_uint(&w, 1); /* SecretStream v1 */
    return w.ok ? capacity - w.left : 0;
}

void hdt_listener_punch_keys(hdt_listener *l, const uint8_t hash[64]) {
    static const uint8_t ns[32] = {
        0xf1, 0x19, 0x1c, 0xd5, 0xe6, 0x7b, 0x10, 0xb5, 0x4a, 0x50, 0x70, 0x33, 0x28, 0x0e, 0xd1, 0xff,
        0x0e, 0x12, 0x27, 0x82, 0x68, 0xd5, 0x67, 0x9c, 0x8f, 0x93, 0xd4, 0x17, 0x21, 0x0d, 0x16, 0x8b
    };
    crypto_generichash(l->punch_secret, 32, ns, sizeof(ns), hash, 64);
    randombytes_buf(l->token_secret, sizeof(l->token_secret));
}

static size_t encode_reply(const hdt_listener *l, const hdt_addr *from, const hdt_addr *peer,
                          uint16_t tid, uint32_t round, const uint8_t token[32],
                          const uint8_t *echo, unsigned error, int punching, uint8_t *packet, size_t capacity) {
    const hdt_addr *mapping = stable_mapping(l);
    uint8_t plain[84], sealed[sizeof(plain) + OVERHEAD], value[sizeof(sealed) + 12];
    writer w = {plain, sizeof(plain), 1};
    write_uint(&w, TOKEN | (echo ? REMOTE_TOKEN : 0) |
                   (mapping ? ADDRESSES : 0) | (punching ? PUNCHING : 0));
    write_uint(&w, error);
    write_uint(&w, mapping ? CONSISTENT : 0);
    write_uint(&w, round);
    if (mapping) {
        write_uint(&w, 1);
        write_addr(&w, *mapping);
    }
    put(&w, token, 32);
    if (echo) put(&w, echo, 32);
    const size_t size = sizeof(plain) - w.left;
    randombytes_buf(sealed, NONCE);
    const int ok = w.ok && !crypto_secretbox_easy(sealed + NONCE, plain, size, sealed, l->punch_secret);
    sodium_memzero(plain, sizeof(plain));
    if (!ok) return 0;

    w = (writer){value, sizeof(value), 1};
    write_uint(&w, 1); /* Peer address present. */
    write_uint(&w, FROM_SERVER);
    write_uint(&w, 0); /* Response ID. */
    write_uint(&w, size + OVERHEAD);
    put(&w, sealed, size + OVERHEAD);
    write_addr(&w, *peer);
    if (!w.ok) return 0;

    hdt_rpc reply = {0};
    reply.type = HDT_RPC_REQUEST;
    reply.flags = HDT_HAS_VALUE | HDT_HAS_TARGET;
    reply.tid = tid;
    reply.observed = *from;
    reply.command = COMMAND;
    reply.target = l->target;
    reply.value = (hdt_slice){value, sizeof(value) - w.left};
    return hdt_rpc_encode(&reply, packet, capacity);
}

size_t hdt_listener_punch_reply(hdt_listener *l, const hdt_addr *from, const hdt_rpc *request,
                               uint8_t *packet, size_t capacity) {
    if (!l->punch_mode || !l->reply_size || l->peer_udx || l->peer_open) return 0;
    reader r = {request->value.data, request->value.size, 1};
    if (read_uint(&r) != 1 || read_uint(&r) != FROM_RELAY || read_uint(&r) != l->punch_id) return 0;
    hdt_slice sealed = read_buffer(&r);
    const hdt_addr peer = read_addr(&r);
    uint8_t plain[160];
    if (!r.ok || r.left || !peer.port || memcmp(peer.ip, l->client.ip, 4) ||
        sealed.size <= OVERHEAD || sealed.size - OVERHEAD > sizeof(plain) ||
        crypto_secretbox_open_easy(plain, sealed.data + NONCE, sealed.size - NONCE,
                                   sealed.data, l->punch_secret)) return 0;

    size_t result = 0;
    r = (reader){plain, sealed.size - OVERHEAD, 1};
    const uint64_t flags = read_uint(&r), error = read_uint(&r);
    const uint64_t firewall = read_uint(&r), round = read_uint(&r);
    uint64_t count = flags & ADDRESSES ? read_uint(&r) : 0;
    int matching_address = 0;
    if (count > 8) r.ok = 0;
    while (r.ok && count--) {
        const hdt_addr candidate = read_addr(&r);
        matching_address |= hdt_addr_equal(&candidate, &peer);
    }
    if (flags & REMOTE_ADDRESS) (void)read_addr(&r);
    const uint8_t *echo = flags & TOKEN ? take(&r, 32) : NULL;
    const uint8_t *remote = flags & REMOTE_TOKEN ? take(&r, 32) : NULL;
    if (!r.ok || r.left || flags > 63 || firewall > 3 || round > UINT32_MAX || round < l->punch_round)
        goto done;

    uint8_t token[32];
    crypto_generichash(token, sizeof(token), peer.ip, sizeof(peer.ip), l->token_secret, 32);
    const int commit = (flags & PUNCHING) != 0;
    const int reject = error != 0 || (commit && (!stable_mapping(l) ||
        (firewall != OPEN && firewall != CONSISTENT) || !matching_address ||
        !remote || sodium_memcmp(remote, token, 32)));
    result = encode_reply(l, from, &peer, request->tid, (uint32_t)round, token, echo,
                          reject ? ABORTED : 0, !reject && (commit || l->punching),
                          packet, capacity);
    if (result) {
        if (reject) {
            hdt_listener_reset(l);
        } else {
            l->punch_round = (uint32_t)round;
            if (commit) {
                l->punching = 1;
                l->lan_ready = 0;
                l->phase.stream.state.stream.transport.peer = peer;
            }
        }
    }
done:
    sodium_memzero(plain, sizeof(plain));
    return result;
}
