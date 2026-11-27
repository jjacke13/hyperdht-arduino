/* SPDX-License-Identifier: LGPL-3.0-only
 * Fixed-buffer adaptation of hyperdht-cpp/src/peer_connect.cpp codecs. */
#include "hdtiny/handshake.h"
#include "compact_internal.h"

size_t hdt_handshake_payload(uint32_t id, uint32_t seq, uint8_t *out, size_t capacity) {
    writer w = {out, capacity, 1};
    if (!out) return 0;
    write_uint(&w, 1); write_uint(&w, 24); /* UDX + SecretStream flags */
    write_uint(&w, 0); write_uint(&w, 1); /* no error; directly reachable */
    write_uint(&w, 1); write_uint(&w, 1); /* UDX v1, reusable socket */
    write_uint(&w, id); write_uint(&w, seq); write_uint(&w, 1);
    return w.ok ? capacity - w.left : 0;
}
static int addresses(reader *r, size_t width) {
    uint64_t count = read_uint(r);
    if (!r->ok || count > r->left / width) return 0;
    take(r, (size_t)count * width);
    return r->ok;
}
int hdt_handshake_peer(const uint8_t *payload, size_t size, hdt_peer_info *out) {
    reader r = {payload, size, 1};
    uint64_t flags, firewall, id, seq;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!payload || read_uint(&r) != 1) return 0;
    flags = read_uint(&r);
    if (!r.ok || flags > 127 || !(flags & 8) || !(flags & 16) || read_uint(&r) != 0) return 0;
    firewall = read_uint(&r);
    if (firewall > 3) return 0;
    if (flags & 1) {
        if (read_uint(&r) > UINT32_MAX || !addresses(&r, 12)) return 0;
    }
    if ((flags & 2) && !addresses(&r, 6)) return 0;
    if ((flags & 4) && !addresses(&r, 18)) return 0;
    if (read_uint(&r) != 1 || read_uint(&r) > 1) return 0;
    id = read_uint(&r); seq = read_uint(&r);
    if (id > UINT32_MAX || seq > UINT32_MAX || read_uint(&r) != 1) return 0;
    if (flags & 32) {
        if (read_uint(&r) != 1 || read_uint(&r) != 0) return 0;
        take(&r, 64);
    }
    if ((flags & 64) && !addresses(&r, 6)) return 0;
    if (!r.ok || r.left) return 0;
    out->firewall = (uint32_t)firewall; out->udx_id = (uint32_t)id; out->udx_seq = (uint32_t)seq;
    return 1;
}
size_t hdt_handshake_wrap(unsigned mode, hdt_slice noise, uint8_t *out, size_t capacity) {
    writer w = {out, capacity, 1};
    if (!out || mode > 4 || !noise.data || !noise.size) return 0;
    write_uint(&w, 0); write_uint(&w, mode);
    write_uint(&w, noise.size); put(&w, noise.data, noise.size);
    return w.ok ? capacity - w.left : 0;
}
int hdt_handshake_unwrap(hdt_slice value, unsigned required_mode, hdt_slice *noise,
                          hdt_addr *peer_address, int *has_peer_address) {
    reader r = {value.data, value.size, 1};
    uint64_t flags;
    hdt_slice view;
    hdt_addr peer = {{0}, 0};
    if (!noise || !peer_address || !has_peer_address) return 0;
    memset(noise, 0, sizeof(*noise)); memset(peer_address, 0, sizeof(*peer_address));
    *has_peer_address = 0;
    if (!value.data) return 0;
    flags = read_uint(&r);
    if (!r.ok || flags > 3 || read_uint(&r) != required_mode) return 0;
    view = read_buffer(&r);
    if (flags & 1) { peer = read_addr(&r); if (!peer.port) return 0; }
    if (flags & 2) { hdt_addr relay = read_addr(&r); if (!relay.port) return 0; }
    if (!r.ok || r.left || !view.size) return 0;
    *noise = view; *peer_address = peer; *has_peer_address = !!(flags & 1);
    return 1;
}
