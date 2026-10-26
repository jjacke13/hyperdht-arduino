#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <HolesailWebServer.h>
#include "esp_random.h"

SET_LOOP_TASK_STACK_SIZE(32768);

#if defined(CONFIG_P2P_WIFI_SSID)
static const char *wifiSSID = CONFIG_P2P_WIFI_SSID;
static const char *wifiPassword = CONFIG_P2P_WIFI_PASSWORD;
#else
// Optional local file defines P2P_WIFI_SSID and P2P_WIFI_PASSWORD.
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define P2P_WIFI_SSID "YOUR_WIFI_SSID"
#define P2P_WIFI_PASSWORD ""
#endif
static const char *wifiSSID = P2P_WIFI_SSID;
static const char *wifiPassword = P2P_WIFI_PASSWORD;
#endif

HolesailWebServer server;
static const char page[] = "<!doctype html><meta name='viewport' content='width=device-width'>"
    "<title>ESP32 P2P</title><h1>Hello from an ESP32.</h1>"
    "<p>This page reached you through Holesail and HyperDHT.</p>";

void setup() {
    Serial.begin(115200);
    if (!wifiSSID[0]) {
        Serial.println("Configure Wi-Fi before starting the example");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID, wifiPassword);
    while (WiFi.status() != WL_CONNECTED) delay(100);

    // Keep the public link across reboots; generate only after Wi-Fi enables RNG.
    Preferences identity;
    uint8_t seed[32];
    if (!identity.begin("p2p-arduino", false)) abort();
    const size_t bytes = identity.getBytesLength("seed");
    if (bytes == 0) {
        esp_fill_random(seed, sizeof(seed));
        if (identity.putBytes("seed", seed, sizeof(seed)) != sizeof(seed)) abort();
    } else if (bytes != sizeof(seed) || identity.getBytes("seed", seed, sizeof(seed)) != sizeof(seed)) abort();
    identity.end();
    const bool started = server.begin(page, seed);
    volatile uint8_t *wipe = seed;
    for (size_t i = 0; i < sizeof(seed); ++i) wipe[i] = 0;
    if (!started) { Serial.println("P2P server could not start"); return; }
    Serial.println(server.link());
}

void loop() {
    server.poll();
    static uint32_t reported;
    if (millis() - reported >= 30000) {
        reported = millis();
        Serial.printf("ready=%d PSRAM used=%u/%u internal free=%u\n", server.ready(),
            unsigned(ESP.getPsramSize() - ESP.getFreePsram()),
            unsigned(ESP.getPsramSize()), unsigned(ESP.getFreeHeap()));
        if (server.ready()) Serial.println(server.link());
    }
    delay(10);
}
