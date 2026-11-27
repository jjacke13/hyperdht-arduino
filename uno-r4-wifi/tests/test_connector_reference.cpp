/* SPDX-License-Identifier: LGPL-3.0-only
 * Complete tiny connector versus original peer codecs/Noise/SecretStream/UDX.
 * This reference peer is a local fixture, not a full routing DHT node. */
#include "hdtiny/connector.h"
#include "hdtiny/discovery.h"
#include "hdtiny/handshake.h"
#include "hyperdht/peer_connect.hpp"
#include "hyperdht/dht_messages.hpp"
#include "hyperdht/secret_stream.hpp"
#include <udx.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <memory>

namespace hn = hyperdht::noise;
namespace hm = hyperdht::messages;
namespace hp = hyperdht::peer_connect;
namespace hc = hyperdht::compact;
struct Write {
    udx_stream_write_t *request;
    std::vector<uint8_t> bytes;
    explicit Write(std::vector<uint8_t> b) : request(static_cast<udx_stream_write_t *>(
        std::calloc(1, static_cast<size_t>(udx_stream_write_sizeof(1))))), bytes(std::move(b)) { assert(request); }
    ~Write() { std::free(request); }
};
struct Session {
    uv_loop_t loop{};
    uv_udp_t client_socket{};
    udx_t udx{};
    udx_socket_t peer_socket{};
    udx_stream_t peer_stream{};
    hdt_connector client{};
    hdt_discovery find{};
    hn::Seed client_seed{}, client_ephemeral{}, peer_seed{};
    hn::Keypair peer_key;
    hdt_addr peer_address{};
    std::unique_ptr<hyperdht::secret_stream::SecretStream> crypto;
    std::vector<std::unique_ptr<Write>> writes;
    std::vector<uint8_t> incoming;
    uint8_t rx[2048]{};
    unsigned peer_messages = 0, client_messages = 0, peer_eof = 0;
    bool header_received = false, started = false;
};
static hdt_addr address(const struct sockaddr *a) {
    assert(a->sa_family == AF_INET);
    const auto *v4 = reinterpret_cast<const struct sockaddr_in *>(a);
    hdt_addr result{}; std::memcpy(result.ip, &v4->sin_addr.s_addr, 4); result.port = ntohs(v4->sin_port);
    return result;
}
static hc::Ipv4Address comp(hdt_addr a) { return {{a.ip[0],a.ip[1],a.ip[2],a.ip[3]}, a.port}; }
static std::array<uint8_t,32> prologue() {
    std::array<uint8_t,33> in{}; std::array<uint8_t,32> out{};
    const char name[] = "hyperswarm/dht";
    crypto_generichash(in.data(),32,reinterpret_cast<const uint8_t *>(name),sizeof(name)-1,nullptr,0);
    crypto_generichash(out.data(),32,in.data(),in.size(),nullptr,0);
    return out;
}
static void codecs() {
    uint8_t bytes[64];
    for (unsigned flags = 0; flags < 128; ++flags) {
        if ((flags & 24) != 24) continue;
        hp::NoisePayload p; p.firewall = hp::FIREWALL_OPEN;
        p.udx = hp::UdxInfo{1,true,UINT32_MAX,UINT32_MAX}; p.has_secret_stream = true;
        if (flags & 1) { hp::HolepunchInfo info; info.id = 99; info.relays.push_back({{{127,0,0,1},123},{{127,0,0,1},456}}); p.holepunch = info; }
        if (flags & 2) p.addresses4.push_back({{127,0,0,1},123});
        if (flags & 4) p.addresses6.push_back({{},123});
        if (flags & 32) p.relay_through = hp::RelayThroughInfo{};
        if (flags & 64) p.relay_addresses.push_back({{127,0,0,1},456});
        auto encoded = hp::encode_noise_payload(p); hdt_peer_info decoded{};
        assert(hdt_handshake_peer(encoded.data(),encoded.size(),&decoded));
        assert(decoded.udx_id == UINT32_MAX && decoded.udx_seq == UINT32_MAX && decoded.firewall == 1);
        if (flags == 24) {
            size_t n = hdt_handshake_payload(UINT32_MAX,UINT32_MAX,bytes,sizeof(bytes));
            assert(n == encoded.size() && !std::memcmp(bytes,encoded.data(),n));
        }
    }
}
static void write_peer(Session *s, std::vector<uint8_t> bytes) {
    auto w = std::make_unique<Write>(std::move(bytes));
    uv_buf_t buf = uv_buf_init(reinterpret_cast<char *>(w->bytes.data()), static_cast<unsigned>(w->bytes.size()));
    assert(udx_stream_write(w->request, &s->peer_stream, &buf, 1,
        [](udx_stream_write_t *, int status, int) { assert(status == 0); }) == 1);
    s->writes.push_back(std::move(w));
}
static std::vector<uint8_t> message(unsigned i) {
    std::vector<uint8_t> result(1024);
    for (size_t n = 0; n < result.size(); ++n) result[n] = static_cast<uint8_t>(n + i);
    return result;
}
static void receive_stream(udx_stream_t *stream, ssize_t n, const uv_buf_t *buf) {
    auto *s = static_cast<Session *>(stream->data);
    if (n == UV_EOF) { ++s->peer_eof; return; }
    assert(n >= 0);
    s->incoming.insert(s->incoming.end(), buf->base, buf->base + n);
    while (s->incoming.size() >= 3) {
        size_t size = s->incoming[0] | size_t(s->incoming[1]) << 8 | size_t(s->incoming[2]) << 16;
        assert(size <= 1041);
        if (s->incoming.size() < size + 3) return;
        if (!s->header_received) {
            assert(s->crypto->receive_header(s->incoming.data()+3,size));
            s->header_received = true;
            /* Two records in one raw write exercise partial-slot consumption. */
            auto a = message(71), b = message(72);
            auto x = s->crypto->encrypt(a.data(),a.size()), y = s->crypto->encrypt(b.data(),b.size());
            x.insert(x.end(), y.begin(), y.end()); write_peer(s,std::move(x));
        } else {
            auto plain = s->crypto->decrypt(s->incoming.data()+3,size);
            assert(plain && *plain == message(s->peer_messages + 1)); ++s->peer_messages;
        }
        s->incoming.erase(s->incoming.begin(),s->incoming.begin()+static_cast<ptrdiff_t>(size+3));
    }
}
static void receive_rpc(udx_socket_t *socket, ssize_t n, const uv_buf_t *buf, const struct sockaddr *from) {
    auto *s = static_cast<Session *>(socket->data);
    hm::Request req; hm::Response unused;
    assert(n > 0 && hm::decode_message(reinterpret_cast<const uint8_t *>(buf->base),
           static_cast<size_t>(n),req,unused) == hm::REQUEST_ID);
    hm::Response reply; reply.tid = req.tid; reply.from.addr = comp(address(from));
    if (req.command == hm::CMD_FIND_PEER) {
        hyperdht::dht_messages::PeerRecord record;
        record.public_key = s->peer_key.public_key; record.relay_addresses.push_back(comp(s->peer_address));
        reply.value = hyperdht::dht_messages::encode_peer_record(record);
    } else {
        assert(req.command == hm::CMD_PEER_HANDSHAKE && req.target && req.value);
        std::array<uint8_t,32> target{};
        crypto_generichash(target.data(),32,s->peer_key.public_key.data(),32,nullptr,0);
        assert(*req.target == target);
        auto hs = hp::decode_handshake_msg(req.value->data(),req.value->size());
        assert(hs.mode == hp::MODE_FROM_CLIENT && !s->crypto);
        auto p = prologue(); hn::NoiseIK noise(false,s->peer_key,p.data(),p.size());
        auto plain = noise.recv(hs.noise.data(),hs.noise.size()); assert(plain);
        auto remote = hp::decode_noise_payload(plain->data(),plain->size());
        assert(remote.udx && remote.has_secret_stream && remote.firewall == hp::FIREWALL_OPEN);
        hp::NoisePayload ours; ours.firewall = hp::FIREWALL_OPEN;
        ours.udx = hp::UdxInfo{1,true,55,0}; ours.has_secret_stream = true;
        auto payload = hp::encode_noise_payload(ours);
        hp::HandshakeMessage response; response.mode = hp::MODE_REPLY;
        response.noise = noise.send(payload.data(),payload.size());
        assert(noise.is_complete()); reply.value = hp::encode_handshake_msg(response);
        s->crypto = std::make_unique<hyperdht::secret_stream::SecretStream>(noise.tx_key(),noise.rx_key(),noise.handshake_hash(),false);
        assert(udx_stream_init(&s->udx,&s->peer_stream,55,nullptr,nullptr) == 0);
        s->peer_stream.data = s;
        assert(udx_stream_set_ack(&s->peer_stream,remote.udx->seq) == 0);
        assert(udx_stream_connect(&s->peer_stream,&s->peer_socket,remote.udx->id,from) == 0);
        assert(udx_stream_read_start(&s->peer_stream,receive_stream) == 0);
    }
    auto encoded = hm::encode_response(reply);
    uv_buf_t response = uv_buf_init(reinterpret_cast<char *>(encoded.data()),static_cast<unsigned>(encoded.size()));
    assert(uv_udp_try_send(&socket->uv_udp,&response,1,from) == static_cast<int>(encoded.size()));
    if (req.command == hm::CMD_PEER_HANDSHAKE) write_peer(s,s->crypto->create_header_message());
}
static void receive_client(uv_udp_t *socket, ssize_t n, const uv_buf_t *buf, const struct sockaddr *from, unsigned flags) {
    auto *s = static_cast<Session *>(socket->data);
    if (!n && !from) return;
    assert(n > 0 && from && !(flags & UV_UDP_PARTIAL));
    auto source = address(from);
    if (!s->started) {
        assert(hdt_find_receive(&s->find,&source,reinterpret_cast<const uint8_t *>(buf->base),static_cast<size_t>(n)));
        assert(s->find.status == HDT_FIND_FOUND);
        assert(hdt_connector_begin(&s->client,&s->find.rendezvous,s->peer_key.public_key.data(),
            s->client_seed.data(),s->client_ephemeral.data(),33,123)); s->started = true;
    } else assert(hdt_connector_receive(&s->client,&source,reinterpret_cast<const uint8_t *>(buf->base),static_cast<size_t>(n)));
}
static void run() {
    Session s;
    for (unsigned i = 0; i < 32; ++i) { s.client_seed[i] = uint8_t(i+1); s.peer_seed[i] = uint8_t(i+51); s.client_ephemeral[i] = uint8_t(i+101); }
    s.peer_key = hn::generate_keypair(s.peer_seed);
    assert(uv_loop_init(&s.loop) == 0);
    sockaddr_in bound{}; assert(uv_ip4_addr("127.0.0.1",0,&bound) == 0);
    assert(udx_init(&s.loop,&s.udx,nullptr) == 0);
    assert(udx_socket_init(&s.udx,&s.peer_socket,nullptr) == 0); s.peer_socket.data = &s;
    assert(udx_socket_bind(&s.peer_socket,reinterpret_cast<sockaddr *>(&bound),0) == 0);
    assert(udx_socket_recv_start(&s.peer_socket,receive_rpc) == 0);
    sockaddr_in actual{}; int length = sizeof(actual);
    assert(uv_udp_getsockname(&s.peer_socket.uv_udp,reinterpret_cast<sockaddr *>(&actual),&length) == 0);
    s.peer_address = address(reinterpret_cast<sockaddr *>(&actual));
    assert(uv_udp_init(&s.loop,&s.client_socket) == 0); s.client_socket.data = &s;
    assert(uv_udp_bind(&s.client_socket,reinterpret_cast<sockaddr *>(&bound),0) == 0);
    assert(uv_udp_recv_start(&s.client_socket,[](uv_handle_t *h,size_t,uv_buf_t *b) {
        auto *state = static_cast<Session *>(h->data); *b = uv_buf_init(reinterpret_cast<char *>(state->rx),sizeof(state->rx));
    },receive_client) == 0);
    assert(hdt_find_begin(&s.find,s.peer_key.public_key.data(),&s.peer_address,1,122,1000));
    unsigned sent = 0, held = 0, drops = 0; bool end_sent = false, peer_end_sent = false;
    auto end = std::make_unique<Write>(std::vector<uint8_t>{});
    uint64_t deadline = uv_hrtime()+UINT64_C(3000000000);
    uint64_t simulated_offset = 0;
    while (true) {
        assert(uv_hrtime() < deadline);
        uint8_t packet[2048]; hdt_addr destination{};
        uint64_t now = uv_hrtime()/1000000 + simulated_offset;
        size_t n = s.started ? hdt_connector_poll(&s.client,now,&destination,packet,sizeof(packet)) :
                              hdt_find_next(&s.find,now,&destination,packet,sizeof(packet));
        if (n) {
            unsigned bit = !s.started ? 1u : s.client.status == HDT_CONNECT_HANDSHAKE ? 2u :
                           (packet[0] == 255 && (packet[2] & UDX_HEADER_DATA)) ? 4u : 0u;
            if (bit && !(drops & bit)) { drops |= bit; simulated_offset += 1000; continue; }
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(destination.port);
            std::memcpy(&to.sin_addr.s_addr,destination.ip,4);
            uv_buf_t bytes = uv_buf_init(reinterpret_cast<char *>(packet),static_cast<unsigned>(n));
            assert(uv_udp_try_send(&s.client_socket,&bytes,1,reinterpret_cast<sockaddr *>(&to)) == static_cast<int>(n));
            if (bit == 4)
                assert(uv_udp_try_send(&s.client_socket,&bytes,1,reinterpret_cast<sockaddr *>(&to)) == static_cast<int>(n));
        }
        uv_run(&s.loop,UV_RUN_NOWAIT);
        if (!s.started) continue;
        assert(s.client.status != HDT_CONNECT_FAILED);
        if (s.client.status == HDT_CONNECT_READY && sent < 2) {
            auto plain = message(sent+1);
            if (hdt_connector_write(&s.client,plain.data(),plain.size())) ++sent;
        }
        size_t size = 0;
        const uint8_t *plain = hdt_connector_read(&s.client,&size);
        if (plain) {
            auto expected = message(s.client_messages+71);
            assert(size == expected.size() && !std::memcmp(plain,expected.data(),size));
            if (++held == 20) { ++s.client_messages; held = 0; hdt_connector_release(&s.client); }
        }
        if (s.peer_messages == 2 && s.client_messages == 2) {
            if (!end_sent) end_sent = hdt_connector_end(&s.client);
            if (!peer_end_sent) {
                assert(udx_stream_write_end(end->request,&s.peer_stream,nullptr,0,
                    [](udx_stream_write_t *,int status,int) { assert(status == 0); }) == 1);
                peer_end_sent = true;
            }
        }
        if (end_sent && s.peer_eof == 1 && s.client.status == HDT_CONNECT_EOF &&
            !s.client.state.stream.transport.queued && (s.peer_stream.status & UDX_STREAM_CLOSED)) break;
    }
    assert(drops == 7);
    assert(udx_socket_close(&s.peer_socket) == 0); uv_close(reinterpret_cast<uv_handle_t *>(&s.client_socket),nullptr);
    uv_run(&s.loop,UV_RUN_DEFAULT); assert(uv_loop_close(&s.loop) == 0);
    hdt_connector_clear(&s.client);
    std::printf("Discovery -> Noise RPC -> stock UDX/SecretStream: 2x1KiB each direction, backpressure, EOF PASS; connector=%zu\n",sizeof(hdt_connector));
}
int main() { assert(sodium_init() >= 0); codecs(); run(); }
