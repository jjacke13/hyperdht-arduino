/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDT_TINY_ECHO_SESSION_H
#define HDT_TINY_ECHO_SESSION_H
#include "hdtiny/listener.h"

/* Keep borrowed plaintext until the one TX slot accepts the entire record. */
static inline int echo_record(hdt_connector *stream) {
    size_t count;
    const uint8_t *plain = hdt_connector_read(stream, &count);
    if (plain) {
        if (!hdt_connector_write(stream, plain, count)) return 0;
        hdt_connector_release(stream);
        return 1;
    }
    if (stream->status == HDT_CONNECT_EOF) (void)hdt_connector_end(stream);
    return 0;
}

static inline int echo_finished(const hdt_connector *stream, size_t outgoing) {
    return !outgoing && stream->status == HDT_CONNECT_EOF && stream->state.stream.transport.local_end;
}
#endif
