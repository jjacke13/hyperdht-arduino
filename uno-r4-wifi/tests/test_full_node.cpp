/* SPDX-License-Identifier: LGPL-3.0-only
 * Actual unmodified HyperDHT node/server/SecretStreamDuplex versus tiny core.
 * Run only through run-loopback-only.sh. */
#include "hdtiny/connector.h"
#include "hyperdht/dht.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

static hdt_addr addr(const sockaddr *p) {
    assert(p->sa_family == AF_INET);
    const auto *a = reinterpret_cast<const sockaddr_in *>(p);
    hdt_addr out{}; std::memcpy(out.ip,&a->sin_addr.s_addr,4); out.port = ntohs(a->sin_port); return out;
}
struct Client { hdt_connector core{}; uint8_t buffer[2048]{}; };
int main(int argc, char **argv) {
    const bool serving = argc == 2 && std::strcmp(argv[1],"--serve") == 0;
    assert(argc == 1 || serving);
    assert(sodium_init() >= 0);
    uv_loop_t loop; assert(uv_loop_init(&loop) == 0);
    hyperdht::DhtOptions options; options.host = "127.0.0.1"; options.ephemeral = true; options.adaptive = false;
    hyperdht::HyperDHT node(&loop,options); assert(node.bind() == 0);
    /* Configure the real observed loopback address; no NAT traversal is needed. */
    node.socket().set_firewalled(false);
    for (int i = 0; i < 3; ++i) node.socket().ring_sampler().add("127.0.0.1",node.port());
    assert(node.remote_address());
    hyperdht::noise::Seed server_seed{}; server_seed.fill(51);
    auto server_key = hyperdht::noise::generate_keypair(server_seed);
    uint8_t seed[32] = {1}, ephemeral[32] = {2}, client_key[32], secret[64];
    crypto_sign_seed_keypair(client_key,secret,seed);
    std::unique_ptr<hyperdht::secret_stream::SecretStreamDuplex> duplex;
    std::shared_ptr<void> socket_owner;
    bool peer_received = false, peer_closed = false;
    auto *server = node.create_server();
    server->listen(server_key,[&](const hyperdht::server::ConnectionInfo &info) {
        assert(!duplex && (serving || !std::memcmp(info.remote_public_key.data(),client_key,32)));
        sockaddr_in destination{};
        assert(uv_ip4_addr(info.peer_address.host_string().c_str(),info.peer_address.port,&destination) == 0);
        udx_stream_connect(info.raw_stream,info.udx_socket ? info.udx_socket : node.socket().socket_handle(),
                            info.remote_udx_id,reinterpret_cast<const sockaddr *>(&destination));
        hyperdht::secret_stream::DuplexHandshake hs{};
        hs.tx_key = info.tx_key; hs.rx_key = info.rx_key; hs.handshake_hash = info.handshake_hash;
        hs.public_key = server_key.public_key; hs.remote_public_key = info.remote_public_key; hs.is_initiator = false;
        socket_owner = info.socket_keepalive;
        duplex = std::make_unique<hyperdht::secret_stream::SecretStreamDuplex>(info.raw_stream,hs,&loop);
        duplex->on_message([&](const uint8_t *data,size_t size) {
            assert(size == 1024 && !peer_received); peer_received = true;
            for (size_t i = 0; i < size; ++i) assert(data[i] == uint8_t(i));
            assert(duplex->write(data,size) == 0); duplex->end();
        });
        duplex->on_close([&](int error) { assert(error == 0); peer_closed = true; });
        duplex->start();
    });
    if (serving) {
        /* A bounded native peer for the UART/UDP emulation bridge. */
        struct Control {
            hyperdht::HyperDHT *node;
            bool *closed;
            uint64_t deadline;
            int result;
        } control{&node,&peer_closed,uv_hrtime()+UINT64_C(45000000000),0};
        uv_timer_t timer; assert(uv_timer_init(&loop,&timer) == 0); timer.data = &control;
        assert(uv_timer_start(&timer,[](uv_timer_t *t) {
            auto *c = static_cast<Control *>(t->data);
            if (!*c->closed && uv_hrtime() < c->deadline) return;
            if (!*c->closed) c->result = 1;
            hyperdht::HyperDHT::DestroyOptions opts; opts.force = true; c->node->destroy(opts);
            uv_timer_stop(t); uv_close(reinterpret_cast<uv_handle_t *>(t),nullptr);
        },5,5) == 0);
        std::printf("READY %u ",node.port());
        for (uint8_t b : server_key.public_key) std::printf("%02x",b);
        std::puts(""); std::fflush(stdout);
        uv_run(&loop,UV_RUN_DEFAULT); duplex.reset(); socket_owner.reset();
        assert(uv_loop_close(&loop) == 0);
        return control.result || !peer_received || !peer_closed ? 1 : 0;
    }
    Client client;
    uv_udp_t socket; assert(uv_udp_init(&loop,&socket) == 0); socket.data = &client;
    sockaddr_in bound{}; assert(uv_ip4_addr("127.0.0.1",0,&bound) == 0);
    assert(uv_udp_bind(&socket,reinterpret_cast<sockaddr *>(&bound),0) == 0);
    assert(uv_udp_recv_start(&socket,[](uv_handle_t *h,size_t,uv_buf_t *b) {
        auto *c = static_cast<Client *>(h->data); *b = uv_buf_init(reinterpret_cast<char *>(c->buffer),sizeof(c->buffer));
    },[](uv_udp_t *h,ssize_t size,const uv_buf_t *b,const sockaddr *from,unsigned flags) {
        if (!size && !from) return;
        assert(size > 0 && from && !(flags & UV_UDP_PARTIAL));
        auto *c = static_cast<Client *>(h->data); auto source = addr(from);
        assert(hdt_connector_receive(&c->core,&source,reinterpret_cast<const uint8_t *>(b->base),static_cast<size_t>(size)));
    }) == 0);
    hdt_addr peer{{127,0,0,1},node.port()};
    assert(hdt_connector_begin(&client.core,&peer,server_key.public_key.data(),seed,ephemeral,33,123));
    uint64_t deadline = uv_hrtime()+UINT64_C(5000000000);
    bool sent = false, got = false, ended = false;
    while (!(got && ended && peer_closed && client.core.status == HDT_CONNECT_EOF && !client.core.state.stream.transport.queued)) {
        assert(uv_hrtime() < deadline && client.core.status != HDT_CONNECT_FAILED);
        uint8_t packet[2048]; hdt_addr destination{};
        size_t n = hdt_connector_poll(&client.core,uv_hrtime()/1000000,&destination,packet,sizeof(packet));
        if (n) {
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(destination.port);
            std::memcpy(&to.sin_addr.s_addr,destination.ip,4);
            uv_buf_t b = uv_buf_init(reinterpret_cast<char *>(packet),static_cast<unsigned>(n));
            assert(uv_udp_try_send(&socket,&b,1,reinterpret_cast<sockaddr *>(&to)) == static_cast<int>(n));
        }
        uv_run(&loop,UV_RUN_NOWAIT);
        if (client.core.status == HDT_CONNECT_READY && !sent) {
            uint8_t plain[1024]; for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = uint8_t(i);
            sent = hdt_connector_write(&client.core,plain,sizeof(plain));
        }
        size_t nread; const uint8_t *plain = hdt_connector_read(&client.core,&nread);
        if (plain) {
            assert(nread == 1024 && !got); for (size_t i = 0; i < nread; ++i) assert(plain[i] == uint8_t(i));
            got = true; hdt_connector_release(&client.core);
        }
        if (got && !ended) ended = hdt_connector_end(&client.core);
    }
    assert(peer_received);
    hdt_connector_clear(&client.core); uv_close(reinterpret_cast<uv_handle_t *>(&socket),nullptr);
    hyperdht::HyperDHT::DestroyOptions destroy; destroy.force = true; node.destroy(destroy);
    uv_run(&loop,UV_RUN_DEFAULT); duplex.reset(); socket_owner.reset();
    assert(uv_loop_close(&loop) == 0);
    std::puts("Actual HyperDHT node/server + SecretStreamDuplex: authenticated 1KiB echo + graceful EOF PASS (isolated loopback)");
}
