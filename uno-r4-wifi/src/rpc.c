/* SPDX-License-Identifier: LGPL-3.0-only
 * Adapted from hyperdht-cpp compact.cpp, messages.cpp and dht_messages.cpp.
 * Replaces allocating envelopes with checked cursors and borrowed views. */
#include "hdtiny/rpc.h"
#include <string.h>

#include "compact_internal.h"

int hdt_addr_equal(const hdt_addr *a, const hdt_addr *b) {
    return a && b && a->port == b->port && memcmp(a->ip, b->ip, 4) == 0;
}
size_t hdt_rpc_encode(const hdt_rpc *m, uint8_t *out, size_t capacity) {
    writer w = {out, capacity, 1};
    if (!m || !out || !capacity || (m->flags & ~31u) ||
        (m->type != HDT_RPC_REQUEST && m->type != HDT_RPC_RESPONSE)) return 0;
    write_le(&w, m->type, 1); write_uint(&w, m->flags);
    write_le(&w, m->tid, 2); write_addr(&w, m->observed);
    if (m->flags & HDT_HAS_ID) put(&w, m->id, 32);
    if (m->flags & HDT_HAS_TOKEN) put(&w, m->token, 32);
    if (m->type == HDT_RPC_REQUEST) {
        write_uint(&w, m->command);
        if (m->flags & HDT_HAS_TARGET) put(&w, m->target, 32);
    } else {
        if (m->flags & HDT_HAS_CLOSER) {
            if (m->closer.size % 6) return 0;
            write_uint(&w, m->closer.size / 6); put(&w, m->closer.data, m->closer.size);
        }
        if (m->flags & HDT_HAS_ERROR) write_uint(&w, m->error);
    }
    if (m->flags & HDT_HAS_VALUE) {
        write_uint(&w, m->value.size); put(&w, m->value.data, m->value.size);
    }
    size_t size = capacity - w.left;
    return w.ok && size <= HDT_RPC_MAX_DATAGRAM ? size : 0;
}
int hdt_rpc_decode(const uint8_t *data, size_t size, hdt_rpc *out) {
    hdt_rpc m = {0};
    reader r = {data, size, 1};
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!data || size < 10 || size > HDT_RPC_MAX_DATAGRAM) return 0;
    m.type = (uint8_t)read_le(&r, 1);
    if (m.type != HDT_RPC_REQUEST && m.type != HDT_RPC_RESPONSE) return 0;
    uint64_t flags = read_uint(&r);
    if (!r.ok || flags > 31) return 0;
    m.flags = (uint8_t)flags; m.tid = (uint16_t)read_le(&r, 2);
    m.observed = read_addr(&r);
    if (m.flags & HDT_HAS_ID) m.id = take(&r, 32);
    if (m.flags & HDT_HAS_TOKEN) m.token = take(&r, 32);
    if (m.type == HDT_RPC_REQUEST) {
        uint64_t cmd = read_uint(&r);
        if (cmd > UINT32_MAX) return 0;
        m.command = (uint32_t)cmd;
        if (m.flags & HDT_HAS_TARGET) m.target = take(&r, 32);
    } else {
        if (m.flags & HDT_HAS_CLOSER) {
            uint64_t count = read_uint(&r);
            if (!r.ok || count > r.left / 6) return 0;
            m.closer.size = (size_t)count * 6;
            m.closer.data = take(&r, m.closer.size);
        }
        if (m.flags & HDT_HAS_ERROR) {
            uint64_t err = read_uint(&r);
            if (err > UINT32_MAX) return 0;
            m.error = (uint32_t)err;
        }
    }
    if (m.flags & HDT_HAS_VALUE) m.value = read_buffer(&r);
    if (!r.ok || r.left) return 0;
    *out = m;
    return 1;
}
int hdt_rpc_closer(const hdt_rpc *m, size_t index, hdt_addr *out) {
    if (!m || !out || !m->closer.data || index >= m->closer.size / 6) return 0;
    reader r = {m->closer.data + index * 6, 6, 1};
    *out = read_addr(&r);
    return r.ok;
}
int hdt_peer_record(hdt_slice value, const uint8_t key[32],
                    hdt_addr *relays, size_t capacity, size_t *count) {
    reader r = {value.data, value.size, 1};
    if (count) *count = 0;
    if (!value.data || !key || !count || value.size < 33) return 0;
    const uint8_t *pk = take(&r, 32);
    uint64_t n = read_uint(&r);
    if (!r.ok || memcmp(pk, key, 32) || n > capacity || n > r.left / 6 || (n && !relays)) return 0;
    reader validate = r;
    for (size_t i = 0; i < (size_t)n; ++i) if (!read_addr(&validate).port) return 0;
    if (!validate.ok || validate.left) return 0;
    for (size_t i = 0; i < (size_t)n; ++i) relays[i] = read_addr(&r);
    *count = (size_t)n;
    return 1;
}
