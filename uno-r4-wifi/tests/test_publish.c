/* SPDX-License-Identifier: LGPL-3.0-only */
#include "hdtiny/publish.h"

#include <assert.h>
#include <sodium.h>
#include <string.h>

int main(void) {
    assert(sodium_init() >= 0);
    const hdt_addr node = {{127, 0, 0, 1}, 5000}, other = {{127, 0, 0, 1}, 5001};
    uint8_t key[32], secret[64], packet[256], id[32] = {1}, token[32] = {2};
    crypto_sign_keypair(key, secret);

    hdt_publish publish;
    hdt_rpc request;
    assert(hdt_publish_begin(&publish, &node, key, HDT_PUBLISH_ANNOUNCE));
    assert(!hdt_publish_poll(&publish, 0, secret, packet, 1));
    size_t size = hdt_publish_poll(&publish, 0, secret, packet, sizeof(packet));
    assert(hdt_rpc_decode(packet, size, &request) && request.command == 2);

    hdt_rpc reply = {0};
    reply.type = HDT_RPC_RESPONSE;
    reply.flags = HDT_HAS_ID | HDT_HAS_TOKEN;
    reply.id = id;
    reply.token = token;
    reply.tid = request.tid + 1;
    reply.observed = node;
    size = hdt_rpc_encode(&reply, packet, sizeof(packet));
    assert(!hdt_publish_receive(&publish, &node, packet, size));
    reply.tid = request.tid;
    size = hdt_rpc_encode(&reply, packet, sizeof(packet));
    assert(!hdt_publish_receive(&publish, &other, packet, size));
    assert(hdt_publish_receive(&publish, &node, packet, size));

    assert(!hdt_publish_poll(&publish, 1, secret, packet, 1));
    size = hdt_publish_poll(&publish, 1, secret, packet, sizeof(packet));
    assert(hdt_rpc_decode(packet, size, &request) && request.command == HDT_PUBLISH_ANNOUNCE);
    assert(request.token && memcmp(request.token, token, sizeof(token)) == 0);
    assert(request.value.size == 98);

    // A token response is not a publication ACK even at the expected TID.
    reply.tid = request.tid;
    size = hdt_rpc_encode(&reply, packet, sizeof(packet));
    assert(!hdt_publish_receive(&publish, &node, packet, size));
    assert(!hdt_publish_poll(&publish, 1000, secret, packet, sizeof(packet)));
    size = hdt_publish_poll(&publish, 1001, secret, packet, sizeof(packet));
    assert(hdt_rpc_decode(packet, size, &request) && request.command == 2);
    assert(sodium_is_zero(publish.token, sizeof(publish.token)));
    assert(hdt_publish_poll(&publish, 2001, secret, packet, sizeof(packet)));
    assert(!hdt_publish_poll(&publish, 3001, secret, packet, sizeof(packet)));
    assert(publish.status == HDT_PUBLISH_FAILED);

    sodium_memzero(secret, sizeof(secret));
    return 0;
}
