/* SPDX-License-Identifier: Apache-2.0
 * Reference-sender regression; links original, unmodified libudx source.
 * This includes udx.c to exercise its private RTO and ACK/SACK paths.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#ifndef HDT_LIBUDX_SOURCE
#error Define HDT_LIBUDX_SOURCE to the pinned libudx src/udx.c file
#endif
#include HDT_LIBUDX_SOURCE
#include "hdtiny/udx_rx.h"
#include <stdio.h>

typedef struct {
    uv_loop_t loop;
    udx_t udx;
    udx_socket_t socket;
    udx_stream_t sender;
    uv_udp_t receiver;
    struct sockaddr_in receiver_addr;
    hdt_udx_rx_t rx;
    uint8_t datagram[2048];
    unsigned received, missing_seen, acked;
    size_t payload_size;
} fixture;

static uint32_t read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void alloc_datagram(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    fixture *f = handle->data;
    (void)suggested;
    *buf = uv_buf_init((char *)f->datagram, sizeof(f->datagram));
}

static void receive(uv_udp_t *handle, ssize_t n, const uv_buf_t *buf,
                    const struct sockaddr *addr, unsigned flags) {
    fixture *f = handle->data;
    const uint8_t *p = (const uint8_t *)buf->base;
    uint32_t seq;
    size_t i;
    if (n == 0 && !addr) return;
    assert(n == (ssize_t)(20 + f->payload_size));
    assert(addr && !(flags & UV_UDP_PARTIAL));
    assert(p[0] == UDX_MAGIC_BYTE && p[1] == UDX_VERSION);
    assert(p[2] == UDX_HEADER_DATA && p[3] == 0 && read32(p + 4) == 2);
    seq = read32(p + 12);
    assert(seq < 4);
    for (i = 20; i < (size_t)n; ++i) assert(p[i] == seq);
    ++f->received;
    /* Drop only the first earliest packet; retain the three later packets. */
    if (seq == 0 && ++f->missing_seen == 1) return;
    assert(hdt_udx_rx_admit(&f->rx, seq, p + 20, (size_t)n - 20) ==
           HDT_UDX_RX_RETAINED);
}

static void on_ack(udx_stream_write_t *req, int status, int unordered) {
    fixture *f = req->stream->data;
    assert(status == 0 && unordered == 0);
    ++f->acked;
}

static void pump_until(fixture *f, unsigned count) {
    uint64_t deadline = uv_hrtime() + UINT64_C(1000000000);
    while (f->received < count) {
        assert(uv_hrtime() < deadline);
        uv_run(&f->loop, UV_RUN_NOWAIT);
    }
    /* Complete uv_udp_send callbacks before manually invoking RTO. */
    uv_run(&f->loop, UV_RUN_NOWAIT);
}

static void feedback(fixture *f, uint32_t credit, int sack) {
    /* Aligned little-endian wire packet, parsed by original process_packet(). */
    uint32_t wire[7] = {0};
    uint8_t *p = (uint8_t *)wire;
    p[0] = UDX_MAGIC_BYTE; p[1] = UDX_VERSION;
    p[2] = sack ? UDX_HEADER_SACK : 0;
    wire[1] = udx__swap_uint32_if_be(1);
    wire[2] = udx__swap_uint32_if_be(credit);
    wire[4] = udx__swap_uint32_if_be(f->rx.ack);
    wire[5] = udx__swap_uint32_if_be(1);
    wire[6] = udx__swap_uint32_if_be(4);
    assert(process_packet(&f->socket, (char *)wire, sack ? 28 : 20,
                          (struct sockaddr *)&f->receiver_addr) == 1);
}

static void run_mtu(unsigned mtu) {
    fixture f;
    struct sockaddr_in bind_addr;
    uint8_t data[4 * HDT_UDX_RX_PAYLOAD];
    udx_stream_write_t *write;
    uv_buf_t buf;
    unsigned i;
    int addr_size = sizeof(f.receiver_addr);
    memset(&f, 0, sizeof(f));
    f.payload_size = mtu - 48;
    hdt_udx_rx_init(&f.rx, 0);
    assert(uv_loop_init(&f.loop) == 0);
    assert(uv_ip4_addr("127.0.0.1", 0, &bind_addr) == 0);
    assert(uv_udp_init(&f.loop, &f.receiver) == 0);
    f.receiver.data = &f;
    assert(uv_udp_bind(&f.receiver, (struct sockaddr *)&bind_addr, 0) == 0);
    assert(uv_udp_getsockname(&f.receiver, (struct sockaddr *)&f.receiver_addr,
                              &addr_size) == 0);
    assert(uv_udp_recv_start(&f.receiver, alloc_datagram, receive) == 0);
    assert(udx_init(&f.loop, &f.udx, NULL) == 0);
    assert(udx_socket_init(&f.udx, &f.socket, NULL) == 0);
    assert(udx_socket_bind(&f.socket, (struct sockaddr *)&bind_addr, 0) == 0);
    assert(udx_stream_init(&f.udx, &f.sender, 1, NULL, NULL) == 0);
    f.sender.data = &f;
    assert(udx_stream_connect(&f.sender, &f.socket, 2,
                              (struct sockaddr *)&f.receiver_addr) == 0);
    /* Pin the two reviewed peer MTUs, bypassing only MTU discovery. */
    f.sender.mtu = mtu;
    f.sender.mtu_max = mtu;
    f.sender.mtu_state = UDX_MTU_STATE_SEARCH_COMPLETE;
    f.sender.mtu_probe_wanted = false;
    f.sender.pkt_capacity = f.payload_size;
    for (i = 0; i < 4; ++i)
        memset(data + i * f.payload_size, (int)i, f.payload_size);
    write = calloc(1, (size_t)udx_stream_write_sizeof(1));
    assert(write);
    buf = uv_buf_init((char *)data, 4 * f.payload_size);
    assert(udx_stream_write(write, &f.sender, &buf, 1, on_ack) == 1);
    pump_until(&f, 4);
    assert(f.rx.ack == 0 && hdt_udx_rx_sack_mask(&f.rx) == 14);
    assert(f.sender.seq == 4 && f.sender.write_queue.len == 0);

    /* Negative control: old RX policy stalls the real retransmission path. */
    feedback(&f, 0, 1);
    assert(f.sender.send_rwnd == 0 && f.sender.sacks == 3);
    assert(!udx__cirbuf_get(&f.sender.outgoing, 1));
    assert(!udx__cirbuf_get(&f.sender.outgoing, 2));
    assert(!udx__cirbuf_get(&f.sender.outgoing, 3));
    udx_rto_timeout(&f.sender.rto_timer);
    assert(f.sender.rto_count == 1 && f.sender.retransmit_count == 0);
    assert(f.sender.retransmit_queue.len == 1);
    assert(f.sender.write_queue.len == 0 && f.rx.ack == 0);

    /* Corrected credit is parsed as a window update, sending queued RTO loss. */
    assert(hdt_udx_rx_credit(&f.rx) == 1452);
    feedback(&f, hdt_udx_rx_credit(&f.rx), 1);
    assert(f.sender.retransmit_count == 1);
    assert(f.sender.retransmit_queue.len == 0);
    pump_until(&f, 5);
    assert(f.missing_seen == 2 && f.rx.ack == 4);
    assert(hdt_udx_rx_sack_mask(&f.rx) == 0);
    assert(hdt_udx_rx_credit(&f.rx) == 0);
    feedback(&f, 0, 0);
    assert(f.acked == 1 && f.sender.remote_acked == 4);
    for (i = 0; i < 4; ++i) {
        const hdt_udx_rx_slot_t *s = hdt_udx_rx_peek(&f.rx);
        assert(s && s->seq == i && s->data[0] == i);
        assert(hdt_udx_rx_release(&f.rx, i));
        assert(hdt_udx_rx_credit(&f.rx) == 1452);
    }
    close_stream(&f.sender, 0);
    assert(udx_socket_close(&f.socket) == 0);
    uv_close((uv_handle_t *)&f.receiver, NULL);
    uv_run(&f.loop, UV_RUN_DEFAULT);
    assert(uv_loop_close(&f.loop) == 0);
    free(write);
    printf("MTU %u: old credit blocks RTO; corrected credit delivers gap, ACK=4\n", mtu);
}

int main(void) {
    run_mtu(1200);
    run_mtu(1500);
    return 0;
}
