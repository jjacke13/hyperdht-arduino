// SPDX-License-Identifier: AGPL-3.0-or-later
#include <Arduino.h>
#include <WiFi.h>
#include <HyperDHTNode.h>

SET_LOOP_TASK_STACK_SIZE(32768);

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define P2P_WIFI_SSID "YOUR_WIFI_SSID"
#define P2P_WIFI_PASSWORD ""
#endif

static HyperDHTNode node;
static hyperdht_stream_t *session;
static size_t remaining;

static void onClose(void *) {
    session = nullptr;
}

static void onData(const uint8_t *data, size_t size, void *) {
    if (!session || !size) return;
    if (size > remaining || hyperdht_stream_write(session, data, size) < 0) {
        remaining = 0;
        hyperdht_stream_close(session);
        return;
    }
    remaining -= size;
}

static void onConnection(const hyperdht_connection_t *connection, void *) {
    if (session) {
        // A handshake accepted before the slot filled still transfers its stream.
        auto *extra = hyperdht_stream_open(node.nativeHandle(), connection,
                                           nullptr, nullptr, nullptr, nullptr);
        if (extra) hyperdht_stream_close(extra);
        return;
    }
    remaining = 1024; // Bound how much this example queues per connection.
    session = hyperdht_stream_open(node.nativeHandle(), connection,
                                   nullptr, onData, onClose, nullptr);
}

static void onListening(void *) {
    Serial.print("Server public key: ");
    for (uint8_t byte : node.keypair()->public_key) Serial.printf("%02x", unsigned(byte));
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(P2P_WIFI_SSID, P2P_WIFI_PASSWORD);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 30000) delay(100);
    if (WiFi.status() != WL_CONNECTED || !node.begin()) {
        Serial.println("Wi-Fi or HyperDHT startup failed");
        return;
    }

    hyperdht_server_t *server = hyperdht_server_create(node.nativeHandle());
    if (server) {
        hyperdht_server_set_firewall(server, [](const uint8_t *, const char *, uint16_t, void *) {
            return session ? 1 : 0;
        }, nullptr);
        hyperdht_server_on_listening(server, onListening, nullptr);
        if (hyperdht_server_listen(server, node.keypair(), onConnection, nullptr) == 0) return;
        hyperdht_server_close(server, nullptr, nullptr);
    }
    node.end();
    Serial.println("HyperDHT listener startup failed");
}

void loop() {
    node.poll();
    delay(10);
}
