// SPDX-License-Identifier: LGPL-3.0-only
#include "HyperDHTServer.h"

#include <WiFiS3.h>
#include <cstring>
#include <new>
#include "hdtiny/listener.h"
#include "hdtiny/publish.h"

extern "C" {
extern uint32_t __StackLimit, __StackTop;
void hdt_ra4m1_install_entropy(void);
}

namespace {
using Status = HyperDHTServer::Status;
enum class Stage : uint8_t { Idle, Resolve, Walk, Announce, Listen, Withdraw, Failed };
constexpr uint16_t ListenPort = 49738;
constexpr uint16_t BootstrapPort = 49737;
constexpr const char *Bootstrap[] = {"node1.hyperdht.org", "node2.hyperdht.org", "node3.hyperdht.org"};

union Workspace { hdt_publish_walk walk; hdt_listener listener; } workspace;
hdt_publish publication;
uint8_t packet[1460], key[32], seed[32], secret[64];
hdt_addr nodes[HDT_PUBLISH_RELAYS], local;
uint8_t mask[4];
size_t nodeCount, resolveIndex, publishIndex, acknowledgements;
uint32_t tick;
uint64_t now, lastRefresh, lastActivity, walkStarted;
Stage stage;
bool publishing, bound, modemFault, startupFailed;

WiFiUDP &udp() { static WiFiUDP socket; return socket; }

hdt_addr address(const IPAddress &ip, uint16_t port) {
    return {{ip[0], ip[1], ip[2], ip[3]}, port};
}

int publicAddress(const hdt_addr *addr) {
    const unsigned a = addr->ip[0], b = addr->ip[1], c = addr->ip[2];
    return addr->port && a && a < 224 && a != 10 && a != 127 &&
           !(a == 100 && b >= 64 && b <= 127) &&
           !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31) &&
           !(a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)))) &&
           !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) &&
           !(a == 203 && b == 0 && c == 113);
}

void finish(Stage result) {
    if (bound && !modemFault) udp().stop();
    sodium_memzero(&workspace, sizeof(workspace));
    sodium_memzero(&publication, sizeof(publication));
    sodium_memzero(packet, sizeof(packet));
    sodium_memzero(seed, sizeof(seed));
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(key, sizeof(key));
    bound = publishing = false;
    stage = result;
}

bool send(const hdt_addr &to, size_t size) {
    return size <= sizeof(packet) && udp().beginPacket(IPAddress(to.ip), to.port) &&
           udp().write(packet, size) == size && udp().endPacket();
}

void startPublish() {
    publishing = true;
    publishIndex = acknowledgements = 0;
    hdt_publish_begin(&publication, &nodes[0], key, stage == Stage::Withdraw
        ? HDT_PUBLISH_WITHDRAW : HDT_PUBLISH_ANNOUNCE);
}

void pollPublish() {
    if (!publishing) {
        if (stage != Stage::Listen || now - lastRefresh < 30000) return;
        startPublish();
    }
    if (publication.status == HDT_PUBLISH_DONE || publication.status == HDT_PUBLISH_FAILED) {
        if (publication.status == HDT_PUBLISH_DONE) {
            ++acknowledgements;
            if (stage != Stage::Withdraw)
                hdt_listener_observe(&workspace.listener, &nodes[publishIndex], &publication.observed);
        }
        if (++publishIndex == nodeCount) {
            publishing = false;
            lastRefresh = now;
            if (stage == Stage::Withdraw) {
                finish(!startupFailed && acknowledgements == nodeCount ? Stage::Idle : Stage::Failed);
            } else if (stage == Stage::Announce) {
                if (acknowledgements) stage = Stage::Listen;
                else {
                    // A relay could have published the key even if its ACK was lost.
                    startupFailed = true;
                    stage = Stage::Withdraw;
                    startPublish();
                }
            }
            return;
        }
        hdt_publish_begin(&publication, &nodes[publishIndex], key, publication.action);
    }
    const size_t size = hdt_publish_poll(&publication, now, secret, packet, sizeof(packet));
    if (size) (void)send(nodes[publishIndex], size);
}

void pollWalk(const hdt_addr &source, int incoming) {
    auto &walk = workspace.walk;
    if (incoming) hdt_publish_walk_receive(&walk, &source, packet, incoming, publicAddress);
    hdt_addr destination;
    const size_t size = hdt_publish_walk_poll(&walk, now, &destination, packet, sizeof(packet));
    if (size) (void)send(destination, size);
    if (!walk.done && now - walkStarted < 20000) return;
    nodeCount = walk.best_count;
    if (!nodeCount) { finish(Stage::Failed); return; }
    for (size_t i = 0; i < nodeCount; ++i) nodes[i] = walk.best[i].addr;
    auto *listener = new (&workspace.listener) hdt_listener;
    hdt_listener_init(listener, seed, key, nodes, nodeCount);
    sodium_memzero(seed, sizeof(seed));
    hdt_listener_set_lan(listener, &local, mask);
    stage = Stage::Announce;
    startPublish();
}

void pollStream(const hdt_addr &source, int incoming) {
    auto &listener = workspace.listener;
    auto &stream = listener.phase.stream;
    if (listener.reply_size && now - lastActivity >= (listener.peer_udx ? 60000u : 18000u))
        hdt_listener_reset(&listener);
    if (incoming && packet[0] == HDT_RPC_REQUEST) {
        const bool active = listener.reply_size != 0;
        const size_t reply = hdt_listener_handshake(&listener, &source, packet, incoming, sizeof(packet));
        if (!active && listener.reply_size) lastActivity = now;
        if (reply && !send(source, reply)) hdt_listener_reset(&listener);
    } else if (incoming && hdt_listener_receive(&listener, &source, packet, incoming)) {
        lastActivity = now;
    }

    hdt_addr destination;
    const size_t size = hdt_listener_poll(&listener, now, &destination, packet, sizeof(packet));
    if (size && !send(destination, size)) hdt_listener_reset(&listener);
    else if (stream.status == HDT_CONNECT_FAILED ||
             (!size && stream.status == HDT_CONNECT_EOF && stream.state.stream.transport.local_end))
        hdt_listener_reset(&listener);
}
} // namespace

bool HyperDHTServer::begin(const uint8_t *identity) {
    if (bound || modemFault) return false;
    stage = Stage::Failed;
    const auto top = reinterpret_cast<uintptr_t>(&__StackTop);
    const auto low = reinterpret_cast<uintptr_t>(&__StackLimit);
    if ((__get_CONTROL() & 2u) || top <= low || top - low < 8192 ||
        WiFi.status() != WL_CONNECTED || std::strcmp(WiFi.firmwareVersion(), "0.6.0") != 0) return false;
    const IPAddress ip = WiFi.localIP();
    if (ip == IPAddress(0, 0, 0, 0)) return false;
    local = address(ip, ListenPort);
    const IPAddress subnet = WiFi.subnetMask();
    for (size_t i = 0; i < sizeof(mask); ++i) mask[i] = subnet[i];
    hdt_ra4m1_install_entropy();
    if (sodium_init() < 0 || !udp().begin(ListenPort)) return false;
    bound = true;
    startupFailed = false;
    if (identity) memcpy(seed, identity, sizeof(seed));
    else randombytes_buf(seed, sizeof(seed));
    crypto_sign_seed_keypair(key, secret, seed);
    nodeCount = resolveIndex = 0;
    tick = millis();
    now = 0;
    stage = Stage::Resolve;
    return true;
}

HyperDHTServer::Status HyperDHTServer::poll() {
    if (!bound) return status();
    const uint32_t current = millis();
    now += current - tick;
    tick = current;
    if (stage == Stage::Resolve) {
        IPAddress ip;
        if (WiFi.hostByName(Bootstrap[resolveIndex], ip)) {
            const hdt_addr node = address(ip, BootstrapPort);
            if (publicAddress(&node)) nodes[nodeCount++] = node;
        }
        if (++resolveIndex == sizeof(Bootstrap) / sizeof(*Bootstrap)) {
            auto *walk = new (&workspace.walk) hdt_publish_walk;
            if (hdt_publish_walk_begin(walk, key, nodes, nodeCount)) {
                walkStarted = now;
                stage = Stage::Walk;
            } else finish(Stage::Failed);
        }
        return status();
    }

    const int incoming = udp().parsePacket();
    hdt_addr source{};
    if (incoming < 0 || static_cast<size_t>(incoming) > sizeof(packet)) {
        modemFault = true;
        finish(Stage::Failed);
        return status();
    }
    if (incoming) {
        const IPAddress ip = udp().remoteIP();
        source = address(ip, udp().remotePort());
        if (!udp().readPacketInto(packet, static_cast<size_t>(incoming))) {
            modemFault = true;
            finish(Stage::Failed);
            return status();
        }
    }
    if (stage == Stage::Walk) pollWalk(source, incoming);
    else {
        const bool response = incoming && packet[0] == HDT_RPC_RESPONSE;
        if (response && publishing) hdt_publish_receive(&publication, &source, packet, incoming);
        if (stage == Stage::Listen) pollStream(source, response ? 0 : incoming);
        pollPublish();
    }
    return status();
}

HyperDHTServer::Status HyperDHTServer::status() {
    switch (stage) {
    case Stage::Idle: return Status::Idle;
    case Stage::Failed: return Status::Failed;
    case Stage::Withdraw: return Status::Stopping;
    case Stage::Listen:
        if (workspace.listener.phase.stream.status == HDT_CONNECT_READY) return Status::Connected;
        if (workspace.listener.phase.stream.status == HDT_CONNECT_EOF) return Status::RemoteEnd;
        return Status::Listening;
    default: return Status::Starting;
    }
}

const uint8_t *HyperDHTServer::publicKey() { return bound ? key : nullptr; }

bool HyperDHTServer::write(const void *data, size_t size) {
    return stage == Stage::Listen && data && size && size <= MaxRecord &&
           hdt_connector_write(&workspace.listener.phase.stream, static_cast<const uint8_t *>(data), size);
}

const uint8_t *HyperDHTServer::read(size_t &size) {
    size = 0;
    return stage == Stage::Listen ? hdt_connector_read(&workspace.listener.phase.stream, &size) : nullptr;
}

void HyperDHTServer::release() {
    if (stage == Stage::Listen) hdt_connector_release(&workspace.listener.phase.stream);
}

bool HyperDHTServer::end() {
    return stage == Stage::Listen && hdt_connector_end(&workspace.listener.phase.stream);
}

void HyperDHTServer::stop() {
    if (!bound || stage == Stage::Withdraw) return;
    if (stage == Stage::Announce || stage == Stage::Listen) {
        hdt_listener_reset(&workspace.listener);
        stage = Stage::Withdraw;
        startPublish();
    } else finish(Stage::Idle);
}
