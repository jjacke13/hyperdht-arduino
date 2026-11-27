// SPDX-License-Identifier: LGPL-3.0-only
#include <HyperDHTServer.h>
#include <WiFiS3.h>

static const char ssid[] = "YOUR_WIFI_SSID";
static const char password[] = "YOUR_WIFI_PASSWORD";
using EchoServer = HyperDHTServer;
using Status = EchoServer::Status;
static Status previous;

void setup() {
    Serial.begin(115200);
    const uint32_t waitStarted = millis();
    while (!Serial && millis() - waitStarted < 10000) delay(10);
    WiFi.setTimeout(15000);
    if (WiFi.begin(ssid, password) != WL_CONNECTED) {
        Serial.println(F("Wi-Fi join failed"));
        return;
    }
    const uint32_t joined = millis();
    while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - joined < 5000) delay(100);
    if (!EchoServer::begin()) Serial.println(F("Could not start HyperDHT listener"));
}

void loop() {
    if (Serial.available() && Serial.read() == 'q') EchoServer::stop();
    const Status status = EchoServer::poll();
    if (status == Status::Failed && previous != status) Serial.println(F("Listener failed"));
    if (status == Status::Listening && previous == Status::Starting) {
        Serial.print(F("PUBLIC_KEY="));
        const uint8_t *key = EchoServer::publicKey();
        for (size_t i = 0; i < 32; ++i) {
            if (key[i] < 16) Serial.print('0');
            Serial.print(key[i], HEX);
        }
        Serial.println();
    }
    previous = status;

    size_t size;
    if (const uint8_t *data = EchoServer::read(size)) {
        if (EchoServer::write(data, size)) EchoServer::release();
    } else if (status == Status::RemoteEnd) {
        (void)EchoServer::end();
    }
    delay(1);
}
