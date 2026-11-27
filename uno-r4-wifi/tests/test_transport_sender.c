/* SPDX-License-Identifier: Apache-2.0
 * Original libudx on one loopback socket, bounded transport on the other.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include HDT_LIBUDX_SOURCE
#include "hdtiny/transport.h"
#include <stdio.h>

typedef struct {
    uv_loop_t loop;
    udx_t udx;
    udx_socket_t socket;
    udx_stream_t stream;
    uv_udp_t tiny_socket;
    hdt_transport tiny;
    uint8_t packet[2048], original_data[3000], tiny_data[1044];
    size_t tiny_received, original_received;
    unsigned acked, eof, dropped;
} test_state;

static hdt_addr address(const struct sockaddr_in *a) {
    hdt_addr out;
    memcpy(out.ip, &a->sin_addr.s_addr, 4); out.port = ntohs(a->sin_port);
    return out;
}
static void alloc_packet(uv_handle_t *handle, size_t n, uv_buf_t *buf) {
    test_state *s = handle->data;
    (void)n;
    *buf = uv_buf_init((char *)s->packet, sizeof(s->packet));
}
static void receive_tiny(uv_udp_t *handle, ssize_t n, const uv_buf_t *buf,
                           const struct sockaddr *from, unsigned flags) {
    test_state *s = handle->data;
    if (!n && !from) return;
    assert(n > 0 && from && from->sa_family == AF_INET && !(flags & UV_UDP_PARTIAL));
    hdt_addr source = address((const struct sockaddr_in *)from);
    assert(hdt_transport_receive(&s->tiny, &source, (const uint8_t *)buf->base, (size_t)n));
}
static void receive_original(udx_stream_t *stream, ssize_t n, const uv_buf_t *buf) {
    test_state *s = stream->data;
    if (n == UV_EOF) { ++s->eof; return; }
    assert(n >= 0 && s->original_received + (size_t)n <= sizeof(s->tiny_data));
    assert(!memcmp(buf->base, s->tiny_data + s->original_received, (size_t)n));
    s->original_received += (size_t)n;
}
static void ack_original(udx_stream_write_t *write, int status, int unordered) {
    test_state *s = write->stream->data;
    assert(status == 0 && !unordered); ++s->acked;
}
static void pump(test_state *s, uint64_t *now, int require_eof) {
    uint64_t deadline = uv_hrtime() + UINT64_C(2000000000);
    uint8_t wire[HDT_TRANSPORT_DATAGRAM_MAX];
    struct sockaddr_storage original;
    int addrlen = sizeof(original);
    assert(uv_udp_getsockname(&s->socket.uv_udp, (struct sockaddr *)&original, &addrlen) == 0);
    for (;;) {
        const hdt_udx_rx_slot_t *slot;
        size_t n = hdt_transport_poll(&s->tiny, *now, wire, sizeof(wire));
        *now += 10;
        assert(!s->tiny.closed && uv_hrtime() < deadline);
        if (n) {
            if ((wire[2] & UDX_HEADER_DATA) && !s->dropped) ++s->dropped;
            else {
                uv_buf_t buf = uv_buf_init((char *)wire, (unsigned)n);
                assert(uv_udp_try_send(&s->tiny_socket, &buf, 1, (struct sockaddr *)&original) == (int)n);
                /* Duplicate retransmitted DATA: original must deliver once. */
                if (wire[2] & UDX_HEADER_DATA)
                    assert(uv_udp_try_send(&s->tiny_socket, &buf, 1, (struct sockaddr *)&original) == (int)n);
            }
        }
        uv_run(&s->loop, UV_RUN_NOWAIT);
        while ((slot = hdt_udx_rx_peek(&s->tiny.incoming))) {
            assert(slot->length <= sizeof(s->original_data) - s->tiny_received);
            assert(!memcmp(slot->data, s->original_data + s->tiny_received, slot->length));
            s->tiny_received += slot->length;
            assert(hdt_udx_rx_release(&s->tiny.incoming, slot->seq));
            hdt_transport_consumed(&s->tiny);
        }
        if (!require_eof && s->acked && !s->tiny.queued &&
            s->tiny_received == sizeof(s->original_data) && s->original_received == sizeof(s->tiny_data)) return;
        if (require_eof && s->eof == 1 && s->tiny.remote_end == 2 && !s->tiny.queued) return;
    }
}
static void loopback(void) {
    test_state s;
    struct sockaddr_in bound, tiny_addr, original_addr;
    int len = sizeof(tiny_addr);
    uint64_t now = 0;
    udx_stream_write_t *write, *end;
    memset(&s, 0, sizeof(s));
    for (size_t i = 0; i < sizeof(s.tiny_data); ++i) s.tiny_data[i] = (uint8_t)i;
    for (size_t i = 0; i < sizeof(s.original_data); ++i) s.original_data[i] = (uint8_t)(i * 7);
    assert(uv_loop_init(&s.loop) == 0);
    assert(uv_ip4_addr("127.0.0.1", 0, &bound) == 0);
    assert(uv_udp_init(&s.loop, &s.tiny_socket) == 0);
    s.tiny_socket.data = &s;
    assert(uv_udp_bind(&s.tiny_socket, (struct sockaddr *)&bound, 0) == 0);
    assert(uv_udp_getsockname(&s.tiny_socket, (struct sockaddr *)&tiny_addr, &len) == 0);
    assert(uv_udp_recv_start(&s.tiny_socket, alloc_packet, receive_tiny) == 0);
    assert(udx_init(&s.loop, &s.udx, NULL) == 0);
    assert(udx_socket_init(&s.udx, &s.socket, NULL) == 0);
    assert(udx_socket_bind(&s.socket, (struct sockaddr *)&bound, 0) == 0);
    len = sizeof(original_addr);
    assert(uv_udp_getsockname(&s.socket.uv_udp, (struct sockaddr *)&original_addr, &len) == 0);
    hdt_addr peer = address(&original_addr);
    hdt_transport_init(&s.tiny, &peer, 1, 2, UINT32_MAX, UINT32_MAX - 1);
    assert(udx_stream_init(&s.udx, &s.stream, 2, NULL, NULL) == 0);
    s.stream.data = &s;
    assert(udx_stream_set_seq(&s.stream, UINT32_MAX - 1) == 0);
    assert(udx_stream_set_ack(&s.stream, UINT32_MAX) == 0);
    assert(udx_stream_connect(&s.stream, &s.socket, 1, (struct sockaddr *)&tiny_addr) == 0);
    assert(udx_stream_read_start(&s.stream, receive_original) == 0);
    write = calloc(1, (size_t)udx_stream_write_sizeof(1));
    end = calloc(1, (size_t)udx_stream_write_sizeof(1));
    assert(write && end);
    uv_buf_t buf = uv_buf_init((char *)s.original_data, sizeof(s.original_data));
    assert(udx_stream_write(write, &s.stream, &buf, 1, ack_original) == 1);
    assert(hdt_transport_write(&s.tiny, s.tiny_data, sizeof(s.tiny_data)));
    pump(&s, &now, 0);
    assert(s.tiny.retries >= 1 && s.tiny.seq == 0);
    assert(hdt_transport_end(&s.tiny));
    assert(udx_stream_write_end(end, &s.stream, NULL, 0, ack_original) == 1);
    pump(&s, &now, 1);
    if (!(s.stream.status & UDX_STREAM_CLOSED)) close_stream(&s.stream, 0);
    assert(udx_socket_close(&s.socket) == 0);
    uv_close((uv_handle_t *)&s.tiny_socket, NULL);
    uv_run(&s.loop, UV_RUN_DEFAULT);
    assert(uv_loop_close(&s.loop) == 0);
    free(write); free(end);
    printf("Bounded transport <-> stock UDX: bidirectional data, loss/duplicate, wrap, EOF PASS; state=%zu\n", sizeof(hdt_transport));
}
int main(void) { loopback(); return 0; }
