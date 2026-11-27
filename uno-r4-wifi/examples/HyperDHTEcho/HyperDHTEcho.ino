// SPDX-License-Identifier: LGPL-3.0-only
#include <HyperDHTClient.h>
#include <WiFiS3.h>

static const char ssid[] = "YOUR_WIFI_SSID";
static const char password[] = "YOUR_WIFI_PASSWORD";
using EchoClient = HyperDHTClient;
using Status = EchoClient::Status;
static bool sent, echoed, ended;
static uint32_t started;

static int hexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool readKey(uint8_t key[32]) {
    char hex[66];
    Serial.setTimeout(30000);
    size_t size = Serial.readBytesUntil('\n', hex, sizeof(hex));
    if (size == 65 && hex[64] == '\r') --size;
    if (size != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        const int high = hexDigit(hex[i * 2]), low = hexDigit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        key[i] = static_cast<uint8_t>(high * 16 + low);
    }
    return true;
}

static void fail(const __FlashStringHelper *message) {
    EchoClient::close();
    Serial.println(message);
}

void setup() {
    Serial.begin(115200);
    const uint32_t waitStarted = millis();
    while (!Serial && millis() - waitStarted < 10000) delay(10);
    WiFi.setTimeout(15000);
    if (WiFi.begin(ssid, password) != WL_CONNECTED) {
        fail(F("Wi-Fi join failed"));
        return;
    }
    const uint32_t joined = millis();
    while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - joined < 5000) delay(100);
    Serial.println(F("Paste the echo server's public key and press Enter"));
    uint8_t key[32];
    if (!readKey(key) || !EchoClient::begin(key)) {
        fail(F("Could not start HyperDHT"));
        return;
    }
    started = millis();
}

void loop() {
    const Status status = EchoClient::poll();
    if (status == Status::Idle) return;
    if (status == Status::Failed || millis() - started >= 45000) {
        fail(F("HyperDHT session failed"));
        return;
    }
    if (status == Status::Finished) {
        Serial.println(echoed ? F("Encrypted 1 KiB echo verified") : F("Peer closed early"));
        EchoClient::close();
        return;
    }
    if (status == Status::Connected && !sent) {
        // Static storage keeps the challenge out of the crypto call stack.
        static uint8_t challenge[EchoClient::MaxRecord];
        for (size_t i = 0; i < sizeof(challenge); ++i) challenge[i] = static_cast<uint8_t>(i);
        sent = EchoClient::write(challenge, sizeof(challenge));
    }
    size_t size;
    if (const uint8_t *data = EchoClient::read(size)) {
        bool match = sent && !echoed && size == EchoClient::MaxRecord;
        for (size_t i = 0; match && i < size; ++i) match = data[i] == static_cast<uint8_t>(i);
        EchoClient::release();
        if (!match) {
            fail(F("Unexpected echo response"));
            return;
        }
        echoed = true;
    }
    if (echoed && !ended) ended = EchoClient::end();
    if (status == Status::RemoteEnd && !echoed) {
        fail(F("Peer closed early"));
        return;
    }
    delay(1);
}
