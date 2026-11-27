// SPDX-License-Identifier: AGPL-3.0-or-later
#include <Arduino.h>
#include <WiFi.h>
#include <HyperDHTNode.h>
#include <cstring>

SET_LOOP_TASK_STACK_SIZE(32768);

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define P2P_WIFI_SSID "YOUR_WIFI_SSID"
#define P2P_WIFI_PASSWORD ""
#endif
static HyperDHTNode node;
static uint8_t peerKey[32];
static hyperdht_stream_t *session;
static const char message[] = "Hello from ESP32-S3";
static size_t received;
static bool prompted, started, sent, echoed, rejected;

static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parseKey(const char *text) {
    for (size_t i = 0; i < sizeof(peerKey); ++i) {
        const int hi = hexDigit(text[2 * i]);
        const int lo = hexDigit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        peerKey[i] = uint8_t(16 * hi + lo);
    }
    return true;
}

static bool readPeerKey() {
    static char line[2 * sizeof(peerKey) + 1];
    static size_t length = 0;
    while (Serial.available()) {
        const int c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            const bool valid = length == sizeof(line) - 1 && parseKey(line);
            length = 0;
            if (valid) return true;
            Serial.println("Expected a 64-digit hexadecimal public key");
        } else if (length < sizeof(line)) {
            line[length++] = char(c);
        }
    }
    return false;
}

static void onConnect(int error, hyperdht_stream_t *stream, void *) {
    session = stream;
    if (error) Serial.printf("Connect failed (%d)\n", error);
}

static void onClose(void *) {
    session = nullptr;
    Serial.println(echoed ? "Authenticated echo verified" : "Stream closed before a matching echo");
}

static void onData(const uint8_t *data, size_t size, void *) {
    if (!session || echoed || rejected) return;
    if (size > sizeof(message) - 1 - received ||
        std::memcmp(message + received, data, size) != 0) {
        rejected = true;
        hyperdht_stream_close(session);
        return;
    }
    received += size;
    if (received == sizeof(message) - 1) {
        echoed = true;
        hyperdht_stream_close(session);
    }
}

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(P2P_WIFI_SSID, P2P_WIFI_PASSWORD);
    const uint32_t connectedSince = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectedSince < 30000) delay(100);
    if (WiFi.status() != WL_CONNECTED || !node.begin()) {
        Serial.println("Wi-Fi or HyperDHT startup failed");
    }
}

void loop() {
    node.poll();
    if (!prompted && node.ready()) {
        prompted = true;
        Serial.println("Paste the server public key and press Enter");
    }
    if (prompted && !started && readPeerKey()) {
        started = true;
        if (hyperdht_connect_and_open_stream(node.nativeHandle(), peerKey, onConnect,
                                             nullptr, onData, onClose, nullptr) < 0) {
            Serial.println("Could not start the connection");
        }
    }
    if (!sent && session && hyperdht_stream_is_open(session)) {
        sent = true;
        if (hyperdht_stream_write(session, reinterpret_cast<const uint8_t *>(message),
                                  sizeof(message) - 1) < 0) {
            hyperdht_stream_close(session);
        }
    }
    delay(10);
}
