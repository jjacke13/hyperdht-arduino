/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_LISTENER_H
#define HDTINY_LISTENER_H
#include "connector.h"
#include "publish_walk.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    union { hdt_connector stream; hdt_noise noise; } phase;
    hdt_addr relays[HDT_PUBLISH_RELAYS], observed[HDT_PUBLISH_RELAYS], client, local, lan_peer;
    uint8_t mask[4];
    uint8_t seed[32], target[32], request_id[32], reply[128];
    uint8_t punch_secret[32], token_secret[32];
    size_t reply_size;
    uint64_t last_probe;
    uint32_t punch_id, punch_round;
    uint8_t relay_count, peer_udx, relayed, peer_open, punching, probe_sent, punch_mode, lan_ready;
} hdt_listener;

/* IPv4 listener for one session. Initialize sodium/platform randomness first.
 * Use one UDP socket for publication, rendezvous, hole punching and UDX. */
int hdt_listener_init(hdt_listener *l, const uint8_t seed[32], const uint8_t key[32],
                       const hdt_addr *relays, size_t count);
/* Offer a LAN address only to an authenticated peer sharing the external IP
 * and advertising a private address on this subnet. */
void hdt_listener_set_lan(hdt_listener *l, const hdt_addr *local, const uint8_t mask[4]);
/* Record an announcement ACK's observed endpoint while no session is active. */
void hdt_listener_observe(hdt_listener *l, const hdt_addr *relay, const hdt_addr *observed);
/* Handles PEER_HANDSHAKE and PEER_HOLEPUNCH. Input and reply share the packet. */
size_t hdt_listener_handshake(hdt_listener *l, const hdt_addr *from, uint8_t *packet,
                              size_t size, size_t capacity);
int hdt_listener_receive(hdt_listener *l, const hdt_addr *from, const uint8_t *packet, size_t size);
size_t hdt_listener_poll(hdt_listener *l, uint64_t now, hdt_addr *to, uint8_t *packet, size_t capacity);
/* Release a session, keeping the listener identity and rendezvous routes. */
void hdt_listener_reset(hdt_listener *l);
void hdt_listener_clear(hdt_listener *l);
#ifdef __cplusplus
}
#endif
#endif
