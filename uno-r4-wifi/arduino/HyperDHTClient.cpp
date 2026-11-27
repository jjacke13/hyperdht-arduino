// SPDX-License-Identifier: LGPL-3.0-only
#include "HyperDHTClient.h"

#include <WiFiS3.h>
#include <cstring>
#include <new>
#include <sodium.h>

extern "C" {
#include "hdtiny/connector.h"
#include "hdtiny/discovery.h"
extern uint32_t __StackLimit, __StackTop;
void hdt_ra4m1_install_entropy(void);
}

namespace {
using Status = HyperDHTClient::Status;
constexpr size_t DatagramMax = 1460;
constexpr uint16_t BootstrapPort = 49737;
constexpr uint16_t ClientPort = 49738;
constexpr const char *Bootstrap[] = {
    "node1.hyperdht.org", "node2.hyperdht.org", "node3.hyperdht.org"
};

union Phase {
    hdt_discovery discovery;
    hdt_connector connector;
};

Phase phase;
WiFiUDP &udp() { static WiFiUDP socket; return socket; }
uint8_t packet[DatagramMax];
uint8_t target[32];
hdt_addr seeds[sizeof(Bootstrap) / sizeof(*Bootstrap)];
size_t seedCount;
hdt_addr local;
uint8_t mask[4];
uint32_t lastTick;
uint64_t elapsed;
bool bound, looking, modemFault, borrowed;
Status state = Status::Idle;

hdt_addr address(const IPAddress &ip, uint16_t port) {
    return {{ip[0], ip[1], ip[2], ip[3]}, port};
}

bool publicAddress(const hdt_addr &addr) {
    const unsigned a = addr.ip[0], b = addr.ip[1], c = addr.ip[2];
    return addr.port && a && a < 224 && a != 10 && a != 127 &&
           !(a == 100 && b >= 64 && b <= 127) &&
           !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31) &&
           !(a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)))) &&
           !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) &&
           !(a == 203 && b == 0 && c == 113);
}

Status finish(Status result) {
    if (bound && !modemFault) udp().stop();
    bound = looking = borrowed = false;
    sodium_memzero(&phase, sizeof(phase));
    sodium_memzero(packet, sizeof(packet));
    sodium_memzero(target, sizeof(target));
    return state = result;
}

bool startConnector() {
    const hdt_addr rendezvous = phase.discovery.rendezvous;
    uint8_t *identity = packet;
    uint8_t *ephemeral = packet + 32;
    randombytes_buf(packet, 64);
    auto *connector = new (&phase.connector) hdt_connector;
    const bool ok = hdt_connector_begin_nat(connector, &rendezvous, target,
        identity, ephemeral, randombytes_random(), static_cast<uint16_t>(randombytes_random()),
        seeds, seedCount, &local, mask);
    sodium_memzero(packet, 64);
    return ok;
}

bool streamReady() {
    return state == Status::Connected || state == Status::RemoteEnd;
}
} // namespace

bool HyperDHTClient::begin(const uint8_t publicKey[32]) {
    if (bound || modemFault || !publicKey) return false;
    state = Status::Failed;
    const auto stackTop = reinterpret_cast<uintptr_t>(&__StackTop);
    const auto stackLimit = reinterpret_cast<uintptr_t>(&__StackLimit);
    if ((__get_CONTROL() & 2u) || stackTop <= stackLimit || stackTop - stackLimit < 8192 ||
        WiFi.status() != WL_CONNECTED || std::strcmp(WiFi.firmwareVersion(), "0.6.0") != 0) return false;
    const IPAddress ip = WiFi.localIP();
    if (ip == IPAddress(0, 0, 0, 0)) return false;
    local = address(ip, ClientPort);
    const IPAddress subnet = WiFi.subnetMask();
    for (size_t i = 0; i < sizeof(mask); ++i) mask[i] = subnet[i];

    seedCount = 0;
    for (const char *name : Bootstrap) {
        IPAddress node;
        if (!WiFi.hostByName(name, node)) continue;
        const hdt_addr candidate = address(node, BootstrapPort);
        if (publicAddress(candidate)) seeds[seedCount++] = candidate;
    }
    if (!seedCount) return false;
    hdt_ra4m1_install_entropy();
    if (sodium_init() < 0 || !udp().begin(ClientPort)) return false;
    bound = true;
    memcpy(target, publicKey, sizeof(target));
    auto *discovery = new (&phase.discovery) hdt_discovery;
    looking = hdt_find_begin(discovery, target, seeds, seedCount,
                             static_cast<uint16_t>(randombytes_random()), 1000) != 0;
    if (!looking) {
        finish(Status::Failed);
        return false;
    }
    lastTick = millis();
    elapsed = 0;
    state = Status::Connecting;
    return true;
}

HyperDHTClient::Status HyperDHTClient::poll() {
    if (!bound || borrowed) return state;
    const uint32_t tick = millis();
    elapsed += tick - lastTick;
    lastTick = tick;
    if (state == Status::Connecting && elapsed >= 35000) return finish(Status::Failed);

    int incoming = udp().parsePacket();
    hdt_addr source{};
    if (incoming < 0 || static_cast<size_t>(incoming) > DatagramMax) {
        modemFault = true;
        return finish(Status::Failed);
    }
    if (incoming) {
        const IPAddress ip = udp().remoteIP();
        source = address(ip, udp().remotePort());
        if (!udp().readPacketInto(packet, static_cast<size_t>(incoming))) {
            // A partial binary reply may leave the modem UART out of sync.
            modemFault = true;
            return finish(Status::Failed);
        }
    }

    hdt_addr destination;
    size_t outgoing = 0;
    if (looking) {
        auto &discovery = phase.discovery;
        if (incoming) hdt_find_receive(&discovery, &source, packet, incoming);
        if (discovery.status == HDT_FIND_FOUND) {
            looking = false;
            if (!startConnector()) return finish(Status::Failed);
            incoming = 0;
        } else {
            outgoing = hdt_find_next(&discovery, elapsed, &destination, packet, sizeof(packet));
            if (discovery.status != HDT_FIND_RUNNING) return finish(Status::Failed);
        }
    }
    if (!looking) {
        auto &connector = phase.connector;
        if (incoming) hdt_connector_receive(&connector, &source, packet, incoming);
        outgoing = hdt_connector_poll(&connector, elapsed, &destination, packet, sizeof(packet));
        if (connector.status == HDT_CONNECT_FAILED) return finish(Status::Failed);
        if (connector.status == HDT_CONNECT_READY) state = Status::Connected;
        if (connector.status == HDT_CONNECT_EOF) {
            state = Status::RemoteEnd;
            const auto &transport = connector.state.stream.transport;
            size_t unread;
            if (!outgoing && transport.local_end && !transport.queued &&
                !hdt_connector_read(&connector, &unread)) return finish(Status::Finished);
        }
    }
    if (outgoing) {
        // DHT and relay hints are untrusted. Only the authenticated LAN peer
        // selected by the connector may bypass the public destination filter.
        if (!publicAddress(destination) &&
            (looking || !hdt_connector_lan_destination(&phase.connector, &destination))) {
            return looking ? state : finish(Status::Failed);
        }
        if (outgoing > DatagramMax || !udp().beginPacket(IPAddress(destination.ip), destination.port) ||
            udp().write(packet, outgoing) != outgoing || !udp().endPacket()) return finish(Status::Failed);
    }
    return state;
}

HyperDHTClient::Status HyperDHTClient::status() { return state; }

bool HyperDHTClient::write(const void *data, size_t size) {
    return streamReady() && data && size && size <= MaxRecord &&
           hdt_connector_write(&phase.connector, static_cast<const uint8_t *>(data), size);
}

const uint8_t *HyperDHTClient::read(size_t &size) {
    size = 0;
    if (!streamReady()) return nullptr;
    const uint8_t *data = hdt_connector_read(&phase.connector, &size);
    borrowed = data != nullptr;
    return data;
}

void HyperDHTClient::release() {
    if (!borrowed) return;
    borrowed = false;
    hdt_connector_release(&phase.connector);
}

bool HyperDHTClient::end() { return streamReady() && hdt_connector_end(&phase.connector); }

void HyperDHTClient::close() { finish(Status::Idle); }
