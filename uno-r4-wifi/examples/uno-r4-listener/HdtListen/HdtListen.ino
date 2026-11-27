// SPDX-License-Identifier: LGPL-3.0-only
#include <WiFiS3.h>
#include <Arduino_LED_Matrix.h>
#include <DataFlashBlockDevice.h>
#include "listener_settings.h"
#include "echo_session.h"
#include <hdtiny/listener.h>
#include <hdtiny/publish.h>
#include <new>

extern "C" void hdt_ra4m1_install_entropy(void);
extern "C" uint32_t __StackLimit, __StackTop;

static constexpr uint16_t ListenPort = 49738;
static constexpr size_t DatagramMax = 1460;
static union {
    hdt_listener listener;
    hdt_publish_walk walk;
} workspace;
static hdt_publish publication;
static WiFiUDP udp;
static uint8_t packet[DatagramMax], key[32], secret[64];
static hdt_addr relays[HDT_PUBLISH_RELAYS];
static size_t relayCount;
static constexpr const char *Bootstrap[] = {
    "node1.hyperdht.org", "node2.hyperdht.org", "node3.hyperdht.org"
};
static bool modemFault, listening, matrixReady, flashing, refreshing;
static ArduinoLEDMatrix matrix;
static uint64_t clockMs, lastRefresh, lastActivity;
static uint32_t tick, flashStarted, echoedRecords;
static size_t refreshIndex, refreshAcks;

static constexpr uint32_t StackPaint = 0xe62ac3b5;

static bool paintStack() {
    if (__get_CONTROL() & 2u) return false;
    const auto low = reinterpret_cast<uintptr_t>(&__StackLimit);
    const auto top = reinterpret_cast<uintptr_t>(&__StackTop);
    const uint32_t irq = __get_PRIMASK();
    __disable_irq();
    const uintptr_t sp = __get_MSP();
    const bool valid = top > low && top - low >= 8192 && low < sp && sp <= top;
    if (valid) {
        for (uintptr_t p = low; p < sp; p += 4)
            *reinterpret_cast<volatile uint32_t *>(p) = StackPaint;
    }
    __set_PRIMASK(irq);
    return valid;
}

static size_t stackUsed() {
    auto p = reinterpret_cast<uintptr_t>(&__StackLimit);
    const auto top = reinterpret_cast<uintptr_t>(&__StackTop);
    while (p < top && *reinterpret_cast<volatile const uint32_t *>(p) == StackPaint) p += 4;
    return top - p;
}

static bool field(char *out, size_t capacity) {
    size_t used = 0;
    const uint32_t start = millis();
    while (millis() - start < 30000) {
        if (!Serial.available()) { delay(1); continue; }
        const int c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') { out[used] = 0; return true; }
        if (c < 32 || c > 126 || c == ',' || used == capacity - 1) return false;
        out[used++] = static_cast<char>(c);
    }
    return false;
}

static hdt_addr address(const IPAddress &ip, uint16_t port) {
    return {{ip[0], ip[1], ip[2], ip[3]}, port};
}

static bool joinWifi(const ListenerSettings &settings) {
    WiFi.setTimeout(15000);
    const int status = settings.password[0] ? WiFi.begin(settings.ssid, settings.password)
                                             : WiFi.begin(settings.ssid);
    const uint32_t start = millis();
    while (status == WL_CONNECTED && millis() - start < 5000) {
        if (WiFi.localIP() != IPAddress(0, 0, 0, 0)) return true;
        delay(100);
    }
    return false;
}

static bool configure(ListenerSettings &settings) {
    auto &flash = DataFlashBlockDevice::getInstance();
    const int slot = loadSettings(flash, settings);
    if (slot == SettingsFailed) {
        Serial.println(F("R4_LISTENER_FAIL storage"));
        return false;
    }
    if (slot >= 0 && joinWifi(settings)) {
        Serial.println(F("R4_LISTENER_SETTINGS loaded"));
        return true;
    }
    sodium_memzero(settings.ssid, sizeof(settings.ssid));
    sodium_memzero(settings.password, sizeof(settings.password));
    Serial.println(F("R4_LISTENER_CONFIG"));
    if (!field(settings.ssid, sizeof(settings.ssid)) || !settings.ssid[0] ||
        !field(settings.password, sizeof(settings.password)) || !joinWifi(settings)) {
        Serial.println(F("R4_LISTENER_FAIL config"));
        return false;
    }
    if (slot == SettingsEmpty) randombytes_buf(settings.seed, sizeof(settings.seed));
    if (!saveSettings(flash, slot, settings)) {
        Serial.println(F("R4_LISTENER_FAIL storage"));
        return false;
    }
    Serial.println(F("R4_LISTENER_SETTINGS saved"));
    return true;
}

static int receive(hdt_addr &source) {
    const int size = udp.parsePacket();
    if (size > 0 && static_cast<size_t>(size) <= sizeof(packet)) {
        const IPAddress ip = udp.remoteIP();
        source = address(ip, udp.remotePort());
        if (udp.readPacketInto(packet, static_cast<size_t>(size))) return size;
    } else if (!size) {
        return 0;
    }
    // A partial binary reply may have left the UART out of sync.
    modemFault = true;
    return -1;
}

static bool send(const hdt_addr &to, size_t size) {
    return size <= sizeof(packet) && udp.beginPacket(IPAddress(to.ip), to.port) &&
           udp.write(packet, size) == size && udp.endPacket();
}

static int publicAddress(const hdt_addr *addr) {
    const unsigned a = addr->ip[0], b = addr->ip[1], c = addr->ip[2];
    return addr->port && a && a < 224 && a != 10 && a != 127 &&
           !(a == 100 && b >= 64 && b <= 127) &&
           !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31) &&
           !(a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)))) &&
           !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) &&
           !(a == 203 && b == 0 && c == 113);
}

static bool findRelays() {
    hdt_addr seeds[sizeof(Bootstrap) / sizeof(*Bootstrap)];
    size_t count = 0;
    for (const char *host : Bootstrap) {
        IPAddress ip;
        if (!WiFi.hostByName(host, ip)) continue;
        const hdt_addr seed = address(ip, 49737);
        if (publicAddress(&seed)) seeds[count++] = seed;
    }
    auto *walk = new (&workspace.walk) hdt_publish_walk;
    if (!hdt_publish_walk_begin(walk, key, seeds, count)) return false;

    const uint32_t start = millis();
    while (!walk->done && millis() - start < 20000) {
        hdt_addr source, destination;
        const int size = receive(source);
        if (size < 0) return false;
        if (size) hdt_publish_walk_receive(walk, &source, packet, size, publicAddress);
        const size_t outgoing = hdt_publish_walk_poll(walk, millis() - start, &destination,
                                                       packet, sizeof(packet));
        if (outgoing && !send(destination, outgoing)) return false;
        if (!size && !outgoing) delay(1);
    }
    relayCount = walk->best_count;
    for (size_t i = 0; i < relayCount; ++i) relays[i] = walk->best[i].addr;
    return relayCount != 0;
}

static bool publishOne(const hdt_addr &relay, hdt_publish_action action) {
    if (!hdt_publish_begin(&publication, &relay, key, action)) return false;
    const uint32_t start = millis();
    while (millis() - start < 4000) {
        hdt_addr source;
        const int size = receive(source);
        if (size < 0) return false;
        if (size) hdt_publish_receive(&publication, &source, packet, static_cast<size_t>(size));
        const size_t outgoing = hdt_publish_poll(&publication, millis() - start, secret,
                                                  packet, sizeof(packet));
        if (outgoing && !send(relay, outgoing)) return false;
        if (publication.status == HDT_PUBLISH_DONE) {
            if (action == HDT_PUBLISH_ANNOUNCE)
                hdt_listener_observe(&workspace.listener, &relay, &publication.observed);
            return true;
        }
        if (publication.status == HDT_PUBLISH_FAILED) return false;
        delay(1);
    }
    return false;
}

static bool publish(hdt_publish_action action) {
    size_t acknowledged = 0;
    for (size_t i = 0; i < relayCount && !modemFault; ++i)
        if (publishOne(relays[i], action)) ++acknowledged;
    if (modemFault) return false;
    return action == HDT_PUBLISH_WITHDRAW ? acknowledged == relayCount : acknowledged != 0;
}

// 3×5 P and 2 glyphs, plus a one-column exclamation point.
static constexpr uint32_t PunchFrame[][4] = {{0x000ceda2, 0xbccd8888, 0xe9000000, 0}};
static constexpr uint32_t BlankFrame[][4] = {{0, 0, 0, 0}};

static void showPunch(bool show) {
    if (!matrixReady) return;
    const uint32_t irq = __get_PRIMASK();
    __disable_irq();
    if (show) matrix.loadSequence(PunchFrame);
    else matrix.loadSequence(BlankFrame);
    matrix.renderFrame(0);
    __set_PRIMASK(irq);
    flashing = show;
    flashStarted = tick;
}

static void resetSession(bool success) {
    Serial.print(F("R4_LISTENER_SESSION echo="));
    Serial.print(success ? F("ok") : F("failed"));
    Serial.print(F(" records="));
    Serial.print(echoedRecords);
    Serial.print(F(" stack_used="));
    Serial.println(stackUsed());
    echoedRecords = 0;
    hdt_listener_reset(&workspace.listener);
}

static void refresh() {
    if (!refreshing) {
        if (clockMs - lastRefresh < 30000) return;
        refreshIndex = refreshAcks = 0;
        refreshing = true;
        hdt_publish_begin(&publication, &relays[0], key, HDT_PUBLISH_ANNOUNCE);
    }
    if (publication.status == HDT_PUBLISH_DONE || publication.status == HDT_PUBLISH_FAILED) {
        if (publication.status == HDT_PUBLISH_DONE) {
            ++refreshAcks;
            hdt_listener_observe(&workspace.listener, &relays[refreshIndex], &publication.observed);
        }
        if (++refreshIndex == relayCount) {
            refreshing = false;
            lastRefresh = clockMs;
            Serial.println(refreshAcks ? F("R4_LISTENER_REFRESH ok") : F("R4_LISTENER_REFRESH failed"));
            return;
        }
        hdt_publish_begin(&publication, &relays[refreshIndex], key, HDT_PUBLISH_ANNOUNCE);
    }
    const size_t outgoing = hdt_publish_poll(&publication, clockMs, secret, packet, sizeof(packet));
    if (outgoing) (void)send(relays[refreshIndex], outgoing);
}

static void stopListener() {
    listening = false;
    showPunch(false);
    const bool withdrawn = !modemFault && publish(HDT_PUBLISH_WITHDRAW);
    hdt_listener_clear(&workspace.listener);
    sodium_memzero(secret, sizeof(secret));
    sodium_memzero(packet, sizeof(packet));
    if (!modemFault) udp.stop();
    Serial.print(F("R4_LISTENER_STOPPED withdraw="));
    Serial.println(withdrawn ? F("ok") : F("failed"));
}

void setup() {
    const bool painted = paintStack();
    Serial.begin(115200);
    const uint32_t start = millis();
    while (!Serial && millis() - start < 10000) delay(10);
    if (!painted || strcmp(WiFi.firmwareVersion(), "0.6.0")) {
        Serial.println(F("R4_LISTENER_FAIL profile"));
        return;
    }
    hdt_ra4m1_install_entropy();
    if (sodium_init() < 0) {
        Serial.println(F("R4_LISTENER_FAIL entropy"));
        return;
    }
    ListenerSettings settings;
    if (!configure(settings)) {
        sodium_memzero(&settings, sizeof(settings));
        return;
    }
    if (!udp.begin(ListenPort)) {
        sodium_memzero(&settings, sizeof(settings));
        Serial.println(F("R4_LISTENER_FAIL config"));
        return;
    }
    crypto_sign_seed_keypair(key, secret, settings.seed);
    sodium_memzero(settings.ssid, sizeof(settings.ssid));
    sodium_memzero(settings.password, sizeof(settings.password));
    const bool found = findRelays();
    auto *listener = new (&workspace.listener) hdt_listener;
    hdt_listener_init(listener, settings.seed, key, relays, relayCount);
    sodium_memzero(&settings, sizeof(settings));
    if (!found) {
        hdt_listener_clear(listener);
        sodium_memzero(secret, sizeof(secret));
        if (!modemFault) udp.stop();
        Serial.println(F("R4_LISTENER_FAIL lookup"));
        return;
    }
    const hdt_addr local = address(WiFi.localIP(), ListenPort);
    const IPAddress subnet = WiFi.subnetMask();
    const uint8_t mask[4] = {subnet[0], subnet[1], subnet[2], subnet[3]};
    hdt_listener_set_lan(listener, &local, mask);
    if (!publish(HDT_PUBLISH_ANNOUNCE)) {
        Serial.println(F("R4_LISTENER_FAIL announce"));
        stopListener();
        return;
    }
    matrixReady = matrix.begin() != 0;
    if (!matrixReady) Serial.println(F("R4_LISTENER_MATRIX failed"));
    char hex[65];
    sodium_bin2hex(hex, sizeof(hex), key, sizeof(key));
    Serial.print(F("PUBLIC_KEY="));
    Serial.println(hex);
    while (Serial.available()) Serial.read();
    tick = millis();
    listening = true;
}

void loop() {
    if (!listening) { delay(250); return; }
    const uint32_t now = millis();
    clockMs += now - tick;
    tick = now;
    if (flashing && tick - flashStarted >= 3000) showPunch(false);
    if (Serial.available() && Serial.read() == 'q') { stopListener(); return; }

    auto &listener = workspace.listener;
    auto &stream = listener.phase.stream;
    if (listener.reply_size && clockMs - lastActivity >= (listener.peer_udx ? 60000u : 18000u))
        resetSession(false);

    hdt_addr source;
    const int size = receive(source);
    if (size < 0) { stopListener(); return; }
    if (size && packet[0] == HDT_RPC_RESPONSE && refreshing) {
        hdt_publish_receive(&publication, &source, packet, size);
    } else if (size && packet[0] == HDT_RPC_REQUEST) {
        const bool active = listener.reply_size != 0;
        const bool lan = listener.lan_ready != 0;
        const size_t reply = hdt_listener_handshake(&listener, &source, packet, size, sizeof(packet));
        if (!active && listener.reply_size) {
            lastActivity = clockMs;
            Serial.println(listener.relayed ? F("R4_LISTENER_RELAYED") : F("R4_LISTENER_DIRECT"));
        }
        if (reply) {
            if (!send(source, reply) && listener.reply_size) resetSession(false);
            else if (!lan && listener.lan_ready) Serial.println(F("R4_LISTENER_LAN"));
        }
    } else if (size) {
        const bool seen = listener.peer_udx != 0;
        if (hdt_listener_receive(&listener, &source, packet, size)) {
            lastActivity = clockMs;
            if (!seen) {
                Serial.println(F("R4_LISTENER_UDX"));
                showPunch(true);
            }
        }
    }

    if (echo_record(&stream)) ++echoedRecords;
    hdt_addr destination;
    const bool alreadyProbed = listener.probe_sent != 0;
    const size_t outgoing = hdt_listener_poll(&listener, clockMs, &destination, packet, sizeof(packet));
    if (listener.reply_size && stream.status == HDT_CONNECT_FAILED) {
        resetSession(false);
    } else if (outgoing) {
        if (!send(destination, outgoing)) resetSession(false);
        else if (!alreadyProbed && listener.probe_sent) Serial.println(F("R4_LISTENER_PUNCH"));
    } else if (echo_finished(&stream, outgoing)) {
        resetSession(true);
    }
    refresh();
    if (!size && !outgoing) delay(1);
}
