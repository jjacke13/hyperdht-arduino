// SPDX-License-Identifier: AGPL-3.0-or-later
// Public metadata and HTTP transport adapted from Ghost Node and holesail-cpp.
#include "HolesailWebServer.h"
extern "C" {
#include "detail/http.h"
}
#include "holesail/keys.hpp"
#include "holesail/record.hpp"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include <cstring>
#include <limits>
#include <new>
#include <string>

struct HolesailWebServer::State {
    enum { Slots = 4, Chunk = 1024 };
    struct Connection {
        State *owner;
        hyperdht_stream_t *stream;
        http_request_t request;
        char header[512];
        size_t headerSize, sent, size;
        int64_t activity;
        bool responding, closing, headerSent;
    };
    HyperDHTNode node;
    hyperdht_server_t *server = nullptr;
    Connection *connections = nullptr;
    const char *page = nullptr;
    size_t pageSize = 0;
    std::string record = holesail::encode_record("127.0.0.1", std::nullopt, 8080);
    char url[62]{};
    uint64_t latestSeq = 0;
    uint32_t previousIP = 0;
    int64_t publishAt = 0;
    bool found = false, same = false, pending = false, published = false, ending = false;

    static void close(Connection *c) {
        if (c->stream && !c->closing) { c->closing = true; hyperdht_stream_close(c->stream); }
    }
    static void drained(hyperdht_stream_t *stream, void *arg) {
        auto *c = static_cast<Connection *>(arg);
        if (c->stream == stream && !c->closing) send(c);
    }
    static void send(Connection *c) {
        if (!c->stream || c->closing) return;
        c->activity = esp_timer_get_time();
        const uint8_t *data;
        size_t size;
        if (!c->headerSent) {
            data = reinterpret_cast<const uint8_t *>(c->header);
            size = c->headerSize;
            c->headerSent = true;
        } else if (!c->request.head && c->sent < c->size) {
            data = reinterpret_cast<const uint8_t *>(c->owner->page) + c->sent;
            size = c->size - c->sent;
            if (size > Chunk) size = Chunk;
            c->sent += size;
        } else { close(c); return; }
        if (hyperdht_stream_write_with_drain(c->stream, data, size, drained, c) < 0) close(c);
    }
    static void opened(void *arg) { static_cast<Connection *>(arg)->activity = esp_timer_get_time(); }
    static void closed(void *arg) { std::memset(arg, 0, sizeof(Connection)); }
    static void received(const uint8_t *data, size_t size, void *arg) {
        auto *c = static_cast<Connection *>(arg);
        if (c->responding || c->closing || !size) return;
        c->activity = esp_timer_get_time();
        const auto result = http_feed(&c->request, data, size);
        if (result == HTTP_MORE) return;
        unsigned status = result == HTTP_READY ? c->request.status : result == HTTP_TOO_LARGE ? 431 : 400;
        // This first server deliberately exposes only one static page.
        if (status == 200 && c->request.route != http_request_t::ROUTE_PAGE) status = 404;
        c->size = status == 200 ? c->owner->pageSize : 0;
        const int n = http_header(c->header, sizeof(c->header), status, "text/html; charset=utf-8", c->size);
        if (n < 0) { close(c); return; }
        c->headerSize = static_cast<size_t>(n);
        c->responding = true;
        send(c);
    }
    static void connected(const hyperdht_connection_t *connection, void *arg) {
        auto *s = static_cast<State *>(arg);
        if (!s->ending) for (unsigned i = 0; i < Slots; ++i) {
            auto *c = &s->connections[i];
            if (c->stream) continue;
            c->owner = s;
            c->activity = esp_timer_get_time();
            c->stream = hyperdht_stream_open(s->node.nativeHandle(), connection, opened, received, closed, c);
            if (!c->stream) std::memset(c, 0, sizeof(*c));
            return;
        }
        auto *extra = hyperdht_stream_open(s->node.nativeHandle(), connection, nullptr, nullptr, nullptr, nullptr);
        if (extra) hyperdht_stream_close(extra);
    }
    static void gotRecord(uint64_t seq, const uint8_t *value, size_t size, const uint8_t[64], void *arg) {
        auto *s = static_cast<State *>(arg);
        if (!value || (s->found && seq < s->latestSeq)) return;
        s->found = true;
        s->latestSeq = seq;
        s->same = size == s->record.size() && std::memcmp(value, s->record.data(), size) == 0;
    }
    static void putDone(int error, void *arg) {
        auto *s = static_cast<State *>(arg);
        s->pending = false;
        s->published = error == 0;
    }
    static void getDone(int, void *arg) {
        auto *s = static_cast<State *>(arg);
        if (s->ending || (s->found && !s->same && s->latestSeq == std::numeric_limits<uint64_t>::max())) {
            s->pending = false;
            return;
        }
        const uint64_t seq = s->found ? s->latestSeq + (s->same ? 0 : 1) : 0;
        if (hyperdht_mutable_put(s->node.nativeHandle(), s->node.keypair(),
                reinterpret_cast<const uint8_t *>(s->record.data()), s->record.size(), seq, putDone, s) < 0)
            s->pending = false;
    }
    void publish() {
        if (pending || ending) return;
        found = same = false;
        latestSeq = 0;
        pending = true;
        if (hyperdht_mutable_get(node.nativeHandle(), node.keypair()->public_key, 0, gotRecord, getDone, this) < 0)
            pending = false;
    }
};

HolesailWebServer::~HolesailWebServer() { end(); }

bool HolesailWebServer::begin(const char *page, const uint8_t *seed) {
    if (state_ || !page) return false;
    auto *s = new(std::nothrow) State;
    if (!s) return false;
    state_ = s;
    s->page = page;
    s->pageSize = std::strlen(page);
    s->connections = static_cast<State::Connection *>(heap_caps_calloc(State::Slots, sizeof(State::Connection), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s->connections)
        s->connections = static_cast<State::Connection *>(heap_caps_calloc(State::Slots, sizeof(State::Connection), MALLOC_CAP_8BIT));
    if (!s->connections || !s->node.begin(seed)) { end(); return false; }
    const auto url = "hs://0000" + holesail::z32_encode(s->node.keypair()->public_key, 32);
    std::memcpy(s->url, url.c_str(), url.size() + 1);
    s->server = hyperdht_server_create(s->node.nativeHandle());
    if (!s->server) { end(); return false; }
    hyperdht_server_set_reusable_socket(s->server, 1);
    if (hyperdht_server_listen(s->server, s->node.keypair(), State::connected, s) < 0) { end(); return false; }
    s->publish();
    s->publishAt = esp_timer_get_time() + 15000000;
    return true;
}

void HolesailWebServer::poll() {
    auto *s = state_;
    if (!s) return;
    s->node.poll();
    const int64_t now = esp_timer_get_time();
    esp_netif_ip_info_t info{};
    auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr && info.ip.addr != s->previousIP) {
        s->previousIP = info.ip.addr;
        hyperdht_server_refresh(s->server);
        s->publishAt = now;
    }
    for (unsigned i = 0; i < State::Slots; ++i)
        if (s->connections[i].stream && now - s->connections[i].activity > 30000000) State::close(&s->connections[i]);
    if (now >= s->publishAt) {
        s->publish();
        s->publishAt = now + (s->published ? 50LL*60*1000000 : 15000000);
    }
}

void HolesailWebServer::end() {
    auto *s = state_;
    if (!s) return;
    s->ending = true;
    if (s->server) hyperdht_server_close(s->server, nullptr, nullptr);
    // Keep callback state alive until the DHT has drained transport/storage work.
    s->node.end();
    heap_caps_free(s->connections);
    delete s;
    state_ = nullptr;
}

bool HolesailWebServer::ready() const {
    return state_ && state_->node.ready() && state_->published && hyperdht_server_is_listening(state_->server);
}
const char *HolesailWebServer::link() const { return state_ ? state_->url : ""; }
