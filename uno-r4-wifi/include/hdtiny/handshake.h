/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_HANDSHAKE_H
#define HDTINY_HANDSHAKE_H
#include "rpc.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t firewall, udx_id, udx_seq;
} hdt_peer_info;
/* Direct reachable profile: version1, OPEN firewall, reusable UDX socket,
 * SecretStream v1. Optional peer fields are validated/skipped without storage.
 * A decoded non-OPEN peer is returned for caller to reject as unsupported. */
size_t hdt_handshake_payload(uint32_t id, uint32_t seq, uint8_t *out, size_t capacity);
int hdt_handshake_peer(const uint8_t *payload, size_t size, hdt_peer_info *out);
/* Outer PEER_HANDSHAKE RPC value. Views borrow the input packet. */
size_t hdt_handshake_wrap(unsigned mode, hdt_slice noise, uint8_t *out, size_t capacity);
int hdt_handshake_unwrap(hdt_slice value, unsigned required_mode, hdt_slice *noise,
                          hdt_addr *peer_address, int *has_peer_address);
#ifdef __cplusplus
}
#endif
#endif
