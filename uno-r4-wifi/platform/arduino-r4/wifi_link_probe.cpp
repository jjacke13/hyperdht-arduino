/* SPDX-License-Identifier: LGPL-3.0-only
 * Link-only reference to the station and IPv4 UDP paths. Never called by setup.
 * The caller supplies the packet buffer, as the tiny connector does. */
#include <WiFiS3.h>

static WiFiUDP udp;

extern "C" void hdt_r4_wifi_link_probe(const char *ssid, const char *passphrase,
                                      IPAddress peer, uint16_t port, uint8_t *packet,
                                      size_t size, IPAddress *source, uint16_t *source_port) {
    if (WiFi.status() != WL_CONNECTED && WiFi.begin(ssid, passphrase) != WL_CONNECTED) return;
    if (udp.begin(port)) {
        if (udp.beginPacket(peer, port) && udp.write(packet, size) == size) udp.endPacket();
        int received = udp.parsePacket();
        if (received > 0) {
            if (static_cast<size_t>(received) <= size) {
                *source = udp.remoteIP();
                *source_port = udp.remotePort();
                udp.read(packet, static_cast<size_t>(received));
            } else {
                udp.flush();
            }
        }
        udp.stop();
    }
    WiFi.disconnect();
}
