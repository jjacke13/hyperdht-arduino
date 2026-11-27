/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDTINY_LISTENER_PUNCH_INTERNAL_H
#define HDTINY_LISTENER_PUNCH_INTERNAL_H
#include "hdtiny/listener.h"

size_t hdt_listener_punch_payload(hdt_listener *l, uint32_t id, uint8_t *out, size_t capacity);
void hdt_listener_punch_keys(hdt_listener *l, const uint8_t hash[64]);
size_t hdt_listener_punch_reply(hdt_listener *l, const hdt_addr *from, const hdt_rpc *request,
                               uint8_t *packet, size_t capacity);
#endif
