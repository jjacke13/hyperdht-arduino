/* SPDX-License-Identifier: LGPL-3.0-only
 * Differential checks against the selected original HyperDHT codecs. */
#include "hdtiny/discovery.h"
#include "hdtiny/record.h"
#include "hyperdht/messages.hpp"
#include "hyperdht/dht_messages.hpp"
#include "hyperdht/secret_stream.hpp"
#include <sodium.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

namespace ref = hyperdht::messages;
namespace comp = hyperdht::compact;
static comp::Ipv4Address address(hdt_addr a) { return {{a.ip[0],a.ip[1],a.ip[2],a.ip[3]}, a.port}; }
int main() {
    assert(sodium_init() >= 0);
    std::array<uint8_t,32> key{}, token{};
    for (size_t i = 0; i < 32; ++i) { key[i] = uint8_t(i); token[i] = uint8_t(255-i); }
    hdt_addr seed{{192,0,2,1},49737}, next{{192,0,2,2},49737};
    uint8_t bytes[2048];
    std::vector<uint8_t> value{0,1,2,0xfd,0xff};
    /* All 32 envelope flag combinations, not just the discovery subset. */
    for (unsigned flags = 0; flags < 32; ++flags) {
        hdt_rpc m{};
        m.type = HDT_RPC_REQUEST; m.flags = uint8_t(flags); m.tid = 0xabcd;
        m.observed = seed; m.command = 0xffffffff; m.id = key.data();
        m.token = token.data(); m.target = key.data(); m.value = {value.data(),value.size()};
        ref::Request original;
        original.tid = m.tid; original.to.addr = address(seed); original.command = m.command;
        if (flags & 1) original.id = key;
        if (flags & 2) original.token = token;
        original.internal = flags & 4;
        if (flags & 8) original.target = key;
        if (flags & 16) original.value = value;
        auto expected = ref::encode_request(original);
        size_t n = hdt_rpc_encode(&m, bytes, sizeof(bytes));
        assert(n == expected.size() && !memcmp(bytes, expected.data(), n));
        hdt_rpc decoded{};
        assert(hdt_rpc_decode(expected.data(),expected.size(),&decoded));
        assert(decoded.flags == flags && decoded.command == m.command);

        m.type = HDT_RPC_RESPONSE; m.error = 0xffffffff;
        uint8_t closer[] = {192,0,2,2,0x49,0xc2};
        m.closer = {closer,sizeof(closer)};
        ref::Response response;
        response.tid = m.tid; response.from.addr = address(seed);
        if (flags & 1) response.id = key;
        if (flags & 2) response.token = token;
        if (flags & 4) response.closer_nodes.push_back(address(next));
        if (flags & 8) response.error = m.error;
        if (flags & 16) response.value = value;
        expected = ref::encode_response(response);
        n = hdt_rpc_encode(&m,bytes,sizeof(bytes));
        assert(n == expected.size() && !memcmp(bytes,expected.data(),n));
        assert(hdt_rpc_decode(expected.data(),expected.size(),&decoded));
        assert(decoded.flags == flags);
    }
    hdt_discovery d{}; hdt_addr dest{};
    assert(hdt_find_begin(&d,key.data(),&seed,1,123,100));
    size_t n = hdt_find_next(&d,0,&dest,bytes,sizeof(bytes));
    ref::Request req; ref::Response unused;
    assert(ref::decode_message(bytes,n,req,unused) == ref::REQUEST_ID);
    assert(req.target && !req.internal && req.command == ref::CMD_FIND_PEER);
    std::array<uint8_t,32> hash{};
    crypto_generichash(hash.data(),32,key.data(),key.size(),nullptr,0);
    assert(*req.target == hash);
    hyperdht::dht_messages::PeerRecord peer;
    peer.public_key = key; peer.relay_addresses.push_back(address(next));
    ref::Response resp;
    resp.tid = req.tid; resp.from.addr = address(next);
    resp.value = hyperdht::dht_messages::encode_peer_record(peer);
    auto reply = ref::encode_response(resp);
    assert(hdt_find_receive(&d,&seed,reply.data(),reply.size()));
    assert(d.status == HDT_FIND_FOUND && hdt_addr_equal(&d.rendezvous,&seed));
    /* Interoperate with the existing crypto implementation in both directions. */
    hyperdht::secret_stream::Key k1{}, k2{};
    hyperdht::noise::Hash hh{};
    k1[0] = 1; k2[0] = 2; hh[0] = 3;
    for (size_t i = 0; i < hh.size(); ++i) hh[i] = uint8_t(i+1);
    for (int initiator = 0; initiator <= 1; ++initiator) {
    hyperdht::secret_stream::SecretStream remote(k2,k1,hh,!initiator,nullptr);
    hdt_record local{}; uint8_t header[59]; size_t consumed = 0;
    assert(hdt_record_init(&local,k1.data(),k2.data(),hh.data(),initiator,header));
    assert(remote.receive_header(header+3,56));
    auto remote_header = remote.create_header_message();
    assert(hdt_record_feed(&local,remote_header.data(),remote_header.size(),&consumed) == HDT_RECORD_INPUT);
    assert(consumed == remote_header.size());
    for (size_t length : {size_t(1),size_t(17),size_t(1024)}) {
        std::vector<uint8_t> plain(length,42);
        auto encrypted = remote.encrypt(plain.data(),plain.size());
        for (size_t i = 0; i < encrypted.size(); ++i) {
            auto result = hdt_record_feed(&local,encrypted.data()+i,1,&consumed);
            assert(consumed == 1);
            assert(result == (i+1 == encrypted.size() ? HDT_RECORD_MESSAGE : HDT_RECORD_INPUT));
        }
        assert(local.plain_size == plain.size() && !memcmp(local.plain,plain.data(),plain.size()));
        hdt_record_release(&local);
        size_t wire_size = hdt_record_seal(&local,plain.data(),plain.size(),bytes,sizeof(bytes));
        assert(wire_size == length+20);
        auto received = remote.decrypt(bytes+3,wire_size-3);
        assert(received && *received == plain);
    }
    hdt_record_clear(&local);
    }
    std::cout << "64 RPC flag vectors, original FIND_PEER, bidirectional SecretStream: PASS\n";
}
