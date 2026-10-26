#include "HolesailWebServer.h"
#include <uv.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "esp_netif.h"

struct hyperdht_s {
    uv_loop_t *loop;
    hyperdht_mutable_cb got{};
    hyperdht_done_cb getDone{}, putDone{};
    void *getArg{}, *putArg{}, *connectionArg{};
    hyperdht_connection_cb connection{};
    std::string record;
    uint64_t seq{};
    unsigned puts{}, refreshes{};
    bool destroyed{};
};
struct hyperdht_server_s { hyperdht_t *node; };
struct hyperdht_stream_s {
    hyperdht_t *node;
    hyperdht_close_cb open{}, close{};
    hyperdht_data_cb data{};
    void *arg{};
    hyperdht_drain_cb drain{};
    void *drainArg{};
    std::string wire;
    bool closed{};
};
static std::vector<hyperdht_t *> nodes;
static std::vector<std::unique_ptr<hyperdht_stream_t>> streams;
static bool failAllocation, failBind, failServer, failWrite;
static int64_t now;
static uint32_t ip = 1;
static unsigned loopCloses;
static const std::array<uint8_t, 32> publicKey = {
    0xc5,0xff,0xfd,0x40,0x7e,0xa7,0xd8,0x30,0x56,0xda,0x8e,0x19,0x3f,0x33,0x96,0x7b,
    0x2d,0x9d,0x42,0xb5,0xf4,0xa9,0xd5,0x47,0x57,0xf0,0x25,0x3a,0x1c,0xe5,0x6a,0x30};

void *heap_caps_calloc(size_t n, size_t size, unsigned) { return failAllocation ? nullptr : calloc(n, size); }
void heap_caps_free(void *p) { free(p); }
int64_t esp_timer_get_time() { return now; }
void *esp_netif_get_handle_from_ifkey(const char *) { return reinterpret_cast<void *>(1); }
int esp_netif_get_ip_info(void *, esp_netif_ip_info_t *info) { info->ip.addr = ip; return 0; }
extern "C" {
int uv_loop_init(uv_loop_t *) { return 0; }
int uv_loop_close(uv_loop_t *) { ++loopCloses; return 0; }
int uv_run(uv_loop_t *loop, uv_run_mode mode) {
    if (mode != UV_RUN_DEFAULT) return 0;
    auto it = std::find_if(nodes.begin(), nodes.end(), [loop](auto *d) { return d->loop == loop; });
    assert(it != nodes.end());
    auto *d = *it;
    assert(d->destroyed);
    if (d->getDone) { auto cb=d->getDone; d->getDone=nullptr; cb(-1,d->getArg); }
    if (d->putDone) { auto cb=d->putDone; d->putDone=nullptr; cb(-1,d->putArg); }
    for (auto &stream : streams) if (stream->node == d && !stream->closed) hyperdht_stream_close(stream.get());
    return 0;
}
void hyperdht_opts_default(hyperdht_opts_t *o) { memset(o,0,sizeof(*o)); }
hyperdht_t *hyperdht_create(uv_loop_t *loop, const hyperdht_opts_t *o) {
    assert(o->use_public_bootstrap);
    auto *d = new hyperdht_t{}; d->loop=loop; nodes.push_back(d); return d;
}
int hyperdht_bind(hyperdht_t *, uint16_t port) { assert(port==0); return failBind ? -1 : 0; }
void hyperdht_default_keypair(const hyperdht_t *, hyperdht_keypair_t *key) { memcpy(key->public_key,publicKey.data(),32); }
void hyperdht_keypair_zero(hyperdht_keypair_t *key) { memset(key,0,sizeof(*key)); }
void hyperdht_destroy(hyperdht_t *d, hyperdht_close_cb cb, void *arg) { d->destroyed=true; if(cb)cb(arg); }
void hyperdht_free(hyperdht_t *d) { assert(d->destroyed); std::erase(nodes,d); delete d; }
int hyperdht_is_bootstrapped(const hyperdht_t *) { return 1; }
hyperdht_server_t *hyperdht_server_create(hyperdht_t *d) { return failServer ? nullptr : new hyperdht_server_t{d}; }
void hyperdht_server_set_reusable_socket(hyperdht_server_t *, int enabled) { assert(enabled==1); }
int hyperdht_server_listen(hyperdht_server_t *server, const hyperdht_keypair_t *, hyperdht_connection_cb cb, void *arg) {
    server->node->connection=cb; server->node->connectionArg=arg; return 0;
}
int hyperdht_server_is_listening(const hyperdht_server_t *) { return 1; }
void hyperdht_server_refresh(hyperdht_server_t *s) { ++s->node->refreshes; }
void hyperdht_server_close(hyperdht_server_t *s, hyperdht_close_cb cb, void *arg) { delete s; if(cb)cb(arg); }
int hyperdht_mutable_get(hyperdht_t *d, const uint8_t *, uint64_t, hyperdht_mutable_cb got, hyperdht_done_cb done, void *arg) {
    assert(!d->getDone); d->got=got; d->getDone=done; d->getArg=arg; return 0;
}
int hyperdht_mutable_put(hyperdht_t *d, const hyperdht_keypair_t *, const uint8_t *value, size_t size, uint64_t seq, hyperdht_done_cb done, void *arg) {
    ++d->puts; d->seq=seq; d->record.assign(reinterpret_cast<const char *>(value),size); d->putDone=done; d->putArg=arg; return 0;
}
hyperdht_stream_t *hyperdht_stream_open(hyperdht_t *d, const hyperdht_connection_t *, hyperdht_close_cb open, hyperdht_data_cb data, hyperdht_close_cb close, void *arg) {
    auto s=std::make_unique<hyperdht_stream_t>(); s->node=d; s->open=open; s->data=data; s->close=close; s->arg=arg;
    auto *raw=s.get(); streams.push_back(std::move(s)); return raw;
}
int hyperdht_stream_write_with_drain(hyperdht_stream_t *s, const uint8_t *data, size_t size, hyperdht_drain_cb cb, void *arg) {
    if (failWrite) return -1;
    assert(!s->closed && !s->drain); s->wire.append(reinterpret_cast<const char *>(data),size); s->drain=cb; s->drainArg=arg; return 0;
}
void hyperdht_stream_close(hyperdht_stream_t *s) {
    assert(!s->closed); s->closed=true; s->drain=nullptr; if(s->close)s->close(s->arg);
}
}

static void finishGet(hyperdht_t *d) { auto cb=d->getDone; d->getDone=nullptr; assert(cb); cb(0,d->getArg); }
static void finishPut(hyperdht_t *d, int error=0) { auto cb=d->putDone; d->putDone=nullptr; assert(cb); cb(error,d->putArg); }
static hyperdht_stream_t *connect(hyperdht_t *d) {
    hyperdht_connection_t info{}; d->connection(&info,d->connectionArg);
    auto *s=streams.back().get(); if(s->open)s->open(s->arg); return s;
}
static void request(hyperdht_stream_t *s, const std::string &text) {
    s->data(reinterpret_cast<const uint8_t *>(text.data()),text.size(),s->arg);
    unsigned limit=100;
    while(s->drain && limit--) { auto cb=s->drain; s->drain=nullptr; cb(s,s->drainArg); }
    assert(limit);
}

int main() {
    HyperDHTNode raw;
    assert(!raw.ready() && !raw.nativeHandle());
    failBind=true; assert(!raw.begin()); failBind=false; assert(nodes.empty());
    assert(raw.begin()); assert(!raw.begin()); raw.poll(); raw.end(); raw.end(); assert(nodes.empty());
    const std::string page(3500,'x');
    HolesailWebServer server;
    assert(!server.begin(nullptr) && !server.ready() && !server.link()[0]);
    failAllocation=true; assert(!server.begin(page.c_str())); failAllocation=false;
    failServer=true; assert(!server.begin(page.c_str())); failServer=false; assert(nodes.empty());
    assert(server.begin(page.c_str())); assert(!server.begin(page.c_str()));
    assert(std::string(server.link())=="hs://0000az994od6w9cdyis4tacu6chsxcs34oii61w7kt4z6y1uw88fpeay");
    auto *d=nodes.back(); assert(!server.ready());
    finishGet(d); assert(d->seq==0 && d->record=="{\"host\":\"127.0.0.1\",\"port\":8080}");
    finishPut(d); assert(server.ready());
    auto *s=connect(d); request(s,"GET / HTTP/1.1\r\nHost: test\r\n\r\n");
    assert(s->closed && s->wire.ends_with(page) && s->wire.starts_with("HTTP/1.1 200"));
    s=connect(d);
    const std::string first="GET / HTTP/1.1\r\nHost:";
    s->data(reinterpret_cast<const uint8_t *>(first.data()),first.size(),s->arg);
    assert(s->wire.empty() && !s->closed);
    request(s," test\r\n\r\n"); assert(s->closed && s->wire.ends_with(page));
    s=connect(d); failWrite=true; request(s,"GET / HTTP/1.1\r\n\r\n");
    failWrite=false; assert(s->closed);
    s=connect(d); request(s,"HEAD / HTTP/1.1\r\n\r\n"); assert(s->closed && s->wire.ends_with("\r\n\r\n"));
    s=connect(d); request(s,"POST /api/pulse HTTP/1.1\r\n\r\n"); assert(s->wire.starts_with("HTTP/1.1 405"));
    s=connect(d); request(s,"GET / HTTP/1.1\r\nContent-Length: 1\r\n\r\n"); assert(s->wire.starts_with("HTTP/1.1 400"));
    std::array<hyperdht_stream_t *,4> slots;
    for(auto &slot:slots)slot=connect(d);
    auto *extra=connect(d); assert(extra->closed);
    now=31000000; server.poll(); for(auto *slot:slots)assert(slot->closed);
    assert(d->refreshes==1);
    auto old=d->record; d->got(7,reinterpret_cast<const uint8_t *>(old.data()),old.size(),nullptr,d->getArg);
    d->got(2,reinterpret_cast<const uint8_t *>("old"),3,nullptr,d->getArg);
    finishGet(d); assert(d->seq==7); finishPut(d,-1); assert(!server.ready());
    now+=50LL*60*1000000; server.poll();
    d->got(8,reinterpret_cast<const uint8_t *>("old"),3,nullptr,d->getArg);
    finishGet(d); assert(d->seq==9); finishPut(d);
    now+=50LL*60*1000000; server.poll();
    d->got(UINT64_MAX,reinterpret_cast<const uint8_t *>("old"),3,nullptr,d->getArg);
    auto count=d->puts; finishGet(d); assert(d->puts==count);
    HolesailWebServer other; assert(other.begin("second"));
    auto *d2=nodes.back(); finishGet(d2); finishPut(d2); assert(other.ready());
    other.end(); assert(nodes.size()==1); // independent callback state
    ip=2; server.poll(); assert(d->refreshes==2 && d->getDone);
    server.end(); // cancellation callbacks must run before state is freed
    assert(nodes.empty() && !server.ready() && !server.link()[0]);
    assert(server.begin("again")); server.end(); assert(nodes.empty());
    assert(loopCloses>=6);
    std::cout<<"Wrapper: lifecycle failures/restart, isolated publication, URL/seq/retry, fragmented-safe HTTP, backpressure, four slots, idle close and IP refresh pass\n";
}
