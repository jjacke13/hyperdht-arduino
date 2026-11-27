/* SPDX-License-Identifier: LGPL-3.0-only */
#include "hdtiny/connector.h"
#include "hdtiny/handshake.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sodium.h>
#include <string.h>

static const hdt_addr peer = {{127,0,0,1}, 12345};
static void codec(void) {
    uint8_t payload[64], wrapped[96], bytes[64] = {1};
    hdt_peer_info info;
    size_t n = hdt_handshake_payload(UINT32_MAX, UINT32_MAX, payload, sizeof(payload));
    assert(n && hdt_handshake_peer(payload,n,&info));
    assert(info.udx_id == UINT32_MAX && info.udx_seq == UINT32_MAX && info.firewall == 1);
    for (size_t i = 0; i < n; ++i) assert(!hdt_handshake_peer(payload,i,&info));
    payload[n] = 0; assert(!hdt_handshake_peer(payload,n+1,&info));
    hdt_slice noise = {bytes,sizeof(bytes)}, view;
    int has_peer;
    hdt_addr address;
    n = hdt_handshake_wrap(4,noise,wrapped,sizeof(wrapped));
    hdt_slice value = {wrapped,n};
    assert(hdt_handshake_unwrap(value,4,&view,&address,&has_peer));
    assert(!has_peer && view.size == noise.size && !memcmp(view.data,noise.data,noise.size));
    assert(!hdt_handshake_unwrap(value,0,&view,&address,&has_peer));
    for (size_t i = 0; i < n; ++i) {
        value.size = i; assert(!hdt_handshake_unwrap(value,4,&view,&address,&has_peer));
    }
}
static void connector(void) {
    hdt_connector c, before;
    uint8_t seed[32] = {1}, ephemeral[32] = {2}, key[32], secret[64], packet[2048], first[2048];
    hdt_addr destination, other = peer;
    size_t size;
    crypto_sign_seed_keypair(key,secret,seed);
    assert(hdt_connector_begin(&c,&peer,key,seed,ephemeral,17,19));
    assert(!hdt_connector_poll(&c,0,&destination,packet,1) && !c.attempts);
    size = hdt_connector_poll(&c,0,&destination,packet,sizeof(packet));
    assert(size && hdt_addr_equal(&destination,&peer)); memcpy(first,packet,size);
    hdt_rpc request;
    assert(hdt_rpc_decode(packet,size,&request) && request.tid == 19 && request.command == 0);
    hdt_rpc response = {0};
    response.type = HDT_RPC_RESPONSE; response.tid = 18;
    size_t n = hdt_rpc_encode(&response,packet,sizeof(packet)); assert(n);
    before = c; ++other.port;
    assert(!hdt_connector_receive(&c,&other,packet,n));
    assert(!hdt_connector_receive(&c,&peer,packet,n));
    assert(!memcmp(&c,&before,sizeof(c)));
    assert(!hdt_connector_write(&c,seed,1));
    assert(!hdt_connector_read(&c,&n) && n == 0);
    assert(!hdt_connector_end(&c));
    assert(!hdt_connector_poll(&c,999,&destination,packet,sizeof(packet)));
    assert(hdt_connector_poll(&c,1000,&destination,packet,sizeof(packet)) == size);
    assert(!memcmp(packet,first,size));
    assert(hdt_connector_poll(&c,2000,&destination,packet,sizeof(packet)) == size);
    assert(!hdt_connector_poll(&c,3000,&destination,packet,sizeof(packet)));
    assert(c.status == HDT_CONNECT_FAILED && sodium_is_zero((const uint8_t *)&c,sizeof(c)));
    assert(!hdt_connector_poll(&c,4000,&destination,packet,sizeof(packet)));
}
static void half_close(void) {
    hdt_connector c = {0}; hdt_record remote;
    uint8_t key[32] = {1}, hash[64] = {2}, a[59], b[59];
    uint8_t end[20] = {255,1,2,0,1}; size_t used;
    hdt_transport_init(&c.state.stream.transport,&peer,1,2,0,0);
    assert(hdt_record_init(&c.state.stream.record,key,key,hash,1,a));
    assert(hdt_record_init(&remote,key,key,hash,0,b));
    assert(hdt_record_feed(&c.state.stream.record,b,59,&used) == HDT_RECORD_INPUT);
    assert(hdt_record_feed(&remote,a,59,&used) == HDT_RECORD_INPUT);
    c.status = HDT_CONNECT_READY;
    assert(hdt_connector_receive(&c,&peer,end,sizeof(end)));
    assert(c.status == HDT_CONNECT_EOF);
    assert(hdt_connector_write(&c,(const uint8_t *)"reply",5));
    hdt_connector_clear(&c); hdt_record_clear(&remote);
}
int main(void) { assert(sodium_init() >= 0); codec(); connector(); half_close(); return 0; }
