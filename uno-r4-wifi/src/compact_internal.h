/* SPDX-License-Identifier: LGPL-3.0-only
 * Shared checked compact cursors adapted from the original rpc.c. */
#ifndef HDT_COMPACT_INTERNAL_H
#define HDT_COMPACT_INTERNAL_H
#include "hdtiny/rpc.h"
#include <string.h>

typedef struct { const uint8_t *p; size_t left; int ok; } reader;
typedef struct { uint8_t *p; size_t left; int ok; } writer;

static inline const uint8_t *take(reader *r, size_t n) {
    const uint8_t *p = r->p;
    if (!r->ok || n > r->left) { r->ok = 0; return NULL; }
    r->p += n; r->left -= n;
    return p;
}
static inline void put(writer *w, const void *p, size_t n) {
    if (!w->ok || n > w->left || (n && !p)) { w->ok = 0; return; }
    if (n) memcpy(w->p, p, n);
    w->p += n; w->left -= n;
}
static inline uint64_t read_le(reader *r, size_t n) {
    const uint8_t *p = take(r, n);
    uint64_t v = 0;
    if (p) for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static inline void write_le(writer *w, uint64_t v, size_t n) {
    uint8_t b[8];
    for (size_t i = 0; i < n; ++i) b[i] = (uint8_t)(v >> (8 * i));
    put(w, b, n);
}
static inline uint64_t read_uint(reader *r) {
    uint64_t first = read_le(r, 1);
    return first <= 0xfc ? first : read_le(r, first == 0xfd ? 2 : first == 0xfe ? 4 : 8);
}
static inline void write_uint(writer *w, uint64_t v) {
    if (v <= 0xfc) write_le(w, v, 1);
    else {
        size_t n = v <= 0xffff ? 2 : v <= UINT32_MAX ? 4 : 8;
        write_le(w, n == 2 ? 0xfd : n == 4 ? 0xfe : 0xff, 1);
        write_le(w, v, n);
    }
}
static inline hdt_addr read_addr(reader *r) {
    hdt_addr a = {{0}, 0};
    const uint8_t *p = take(r, 4);
    if (p) memcpy(a.ip, p, 4);
    a.port = (uint16_t)read_le(r, 2);
    return a;
}
static inline void write_addr(writer *w, hdt_addr a) {
    put(w, a.ip, 4); write_le(w, a.port, 2);
}
static inline hdt_slice read_buffer(reader *r) {
    hdt_slice b = {NULL, 0};
    uint64_t n = read_uint(r);
    if (!r->ok || n > r->left) { r->ok = 0; return b; }
    b.size = (size_t)n; b.data = take(r, b.size);
    return b;
}

#endif
