/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HDTINY_TRANSPORT_H
#define HDTINY_TRANSPORT_H
#include "rpc.h"
#include "udx_rx.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { HDT_TRANSPORT_TX_MAX = 1044, HDT_TRANSPORT_DATAGRAM_MAX = 1472 };
typedef struct {
    hdt_udx_rx_t incoming;
    hdt_addr peer;
    uint8_t tx[HDT_TRANSPORT_TX_MAX];
    uint32_t local_id, remote_id, seq, peer_credit, end_seq, rto;
    uint32_t window_seq, window_ack;
    uint64_t last_send;
    size_t tx_size;
    uint8_t queued, sent, tx_end, local_end, remote_end, end_data, ack_due, retries, closed;
} hdt_transport;
/* Fixed IPv4 peer; source migration and unordered application messages disabled.
 * One stream, one outstanding TX, fixed 1200-byte outbound MTU, no MTU search.
 * Caller supplies negotiated UDX IDs and starting sequence numbers. */
void hdt_transport_init(hdt_transport *t, const hdt_addr *peer, uint32_t local_id,
                         uint32_t remote_id, uint32_t send_seq, uint32_t recv_seq);
/* Copy one nonempty stream chunk, retaining it until cumulative ACK. End queues
 * a standalone reliable END after the last chunk has been acknowledged. */
int hdt_transport_write(hdt_transport *t, const uint8_t *data, size_t size);
int hdt_transport_end(hdt_transport *t);
/* Caller sends returned bytes to t->peer from the same UDP socket; a failed
 * platform send remains outstanding and is retried. Use monotonic milliseconds.
 * Poll even while idle so ACK/window updates and retransmissions can run. */
size_t hdt_transport_poll(hdt_transport *t, uint64_t now_ms, uint8_t *out, size_t capacity);
/* Invalid/foreign datagrams return 0 without state change; valid packets return
 * 1, including bounded-pool drops. closed is terminal (destroy/retry exhaustion). */
int hdt_transport_receive(hdt_transport *t, const hdt_addr *source,
                           const uint8_t *data, size_t size);
/* Peek/release ordered ciphertext through incoming. Call consumed after each
 * release to schedule a window update. remote_end==2 means ordered EOF reached.
 * END delivery waits for queued payloads to be consumed. */
void hdt_transport_consumed(hdt_transport *t);
void hdt_transport_clear(hdt_transport *t);
#ifdef __cplusplus
}
#endif
#endif
