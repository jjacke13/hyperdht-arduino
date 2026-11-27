#include <WiFiS3.h>
#include "session.h"
extern "C" void sodium_memzero(void *, size_t);
extern "C" uint32_t __StackLimit, __StackTop, __HeapBase, __HeapLimit;
extern "C" void *__real__sbrk(int);
extern "C" [[noreturn]] void __real_abort();

static const uint16_t bootstrap_port = 49737, board_port = 49738;
static const char *const bootstrap[] = {
    "node1.hyperdht.org", "node2.hyperdht.org", "node3.hyperdht.org"
};
static const size_t datagram_max = 1460;
static const uint32_t paint = 0xe62ac3b5;
static size_t heap_peak;
static unsigned allocations_failed;
static int last_failed_growth;
static volatile bool tracing_read;
static WiFiUDP udp;

extern "C" void *__wrap__sbrk(int increment) {
    void *previous = __real__sbrk(increment);
    if (increment <= 0) return previous;
    if (previous == reinterpret_cast<void *>(-1)) {
        ++allocations_failed;
        last_failed_growth = increment;
    } else {
        const uintptr_t end = reinterpret_cast<uintptr_t>(previous) +
                              ((static_cast<unsigned>(increment) + 3u) & ~3u);
        const size_t bytes = end - reinterpret_cast<uintptr_t>(&__HeapBase);
        if (bytes > heap_peak) heap_peak = bytes;
    }
    return previous;
}

extern "C" [[noreturn]] void __wrap_abort() {
    if (tracing_read && __get_PRIMASK() == 0) {
        Serial.print(F("R4_READ_ABORT oom="));
        Serial.print(allocations_failed);
        Serial.print(F(" failed_growth="));
        Serial.print(last_failed_growth);
        Serial.print(F(" heap_high="));
        Serial.println(heap_peak);
        Serial.flush();
    }
    __real_abort();
}

static bool paint_stack() {
    if (__get_CONTROL() & 2u) return false;
    const uint32_t irq = __get_PRIMASK();
    __disable_irq();
    const uintptr_t sp = __get_MSP();
    const uintptr_t low = reinterpret_cast<uintptr_t>(&__StackLimit);
    const uintptr_t top = reinterpret_cast<uintptr_t>(&__StackTop);
    if (low < sp && sp <= top) {
        for (uintptr_t p = low; p < sp; p += 4) *reinterpret_cast<volatile uint32_t *>(p) = paint;
    }
    __set_PRIMASK(irq);
    return low < sp && sp <= top;
}

static size_t stack_used() {
    uintptr_t p = reinterpret_cast<uintptr_t>(&__StackLimit);
    const uintptr_t top = reinterpret_cast<uintptr_t>(&__StackTop);
    while (p < top && *reinterpret_cast<volatile const uint32_t *>(p) == paint) p += 4;
    return top - p;
}

static void report(const char *stage, int result) {
    const size_t stack = stack_used();
    Serial.print(stage);
    Serial.print(' ');
    Serial.print(result);
    Serial.print(F(" heap_high="));
    Serial.print(heap_peak);
    Serial.print(F(" oom="));
    Serial.print(allocations_failed);
    Serial.print(F(" stack_used="));
    Serial.println(stack);
}

static bool field(char *out, size_t capacity, bool empty_ok) {
    size_t count = 0;
    const uint32_t start = millis();
    while (millis() - start < 10000) {
        if (!Serial.available()) { delay(1); continue; }
        const int c = Serial.read();
        if (c == '\n') {
            out[count] = 0;
            return empty_ok || count != 0;
        }
        // WiFiS3 passes these fields to the modem without escaping.
        if (c < 32 || c > 126 || c == ',' || count + 1 == capacity) return false;
        out[count++] = static_cast<char>(c);
    }
    return false;
}

static int join() {
    // Reuse the same packet workspace that the connector self-check just cleared.
    char *ssid = reinterpret_cast<char *>(hdt_r4_bench_packet());
    char *password = ssid + 33;
    int status = -1;
    if (field(ssid, 33, false) && field(password, 64, true)) {
        Serial.print(F("R4_MODEM version="));
        Serial.println(WiFi.firmwareVersion());
        WiFi.setTimeout(15000);
        const uint32_t start = millis();
        status = *password ? WiFi.begin(ssid, password) : WiFi.begin(ssid);
        Serial.print(F("R4_JOIN elapsed_ms="));
        Serial.print(millis() - start);
        Serial.print(F(" modem_status="));
        Serial.println(WiFi.status());
    }
    sodium_memzero(ssid, 97);
    return status;
}

static hdt_addr address(const IPAddress &ip, uint16_t port) {
    return {{ip[0], ip[1], ip[2], ip[3]}, port};
}

static bool public_address(const hdt_addr &addr) {
    const unsigned a = addr.ip[0], b = addr.ip[1], c = addr.ip[2];
    return addr.port && a && a < 224 && a != 10 && a != 127 &&
           !(a == 100 && b >= 64 && b <= 127) &&
           !(a == 169 && b == 254) && !(a == 172 && b >= 16 && b <= 31) &&
           !(a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)))) &&
           !(a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) &&
           !(a == 203 && b == 0 && c == 113);
}

static int session(const IPAddress &local_ip) {
    const IPAddress local_mask = WiFi.subnetMask();
    const hdt_addr local = address(local_ip, board_port);
    const uint8_t mask[4] = {local_mask[0], local_mask[1], local_mask[2], local_mask[3]};
    hdt_r4_bench_set_lan(&local, mask);
    uint8_t *packet = hdt_r4_bench_packet();
    Serial.println(F("R4_KEY_READY"));
    Serial.setTimeout(8000);
    if (Serial.readBytes(packet, 32) != 32) return 1;
    hdt_addr seeds[sizeof(bootstrap) / sizeof(*bootstrap)];
    size_t count = 0;
    for (const char *hostname : bootstrap) {
        IPAddress ip;
        if (!WiFi.hostByName(hostname, ip)) continue;
        const hdt_addr seed = address(ip, bootstrap_port);
        if (public_address(seed)) seeds[count++] = seed;
    }
    if (!count || !hdt_r4_bench_begin(packet, seeds, count)) return 7;
    report("R4_LOOKUP", static_cast<int>(count));
    const uint32_t start = millis();
    bool finding = true, reported_lan = false;
    while (millis() - start < 35000) {
        int incoming = udp.parsePacket();
        hdt_addr source{};
        if (incoming < 0 || static_cast<size_t>(incoming) > datagram_max) return 2;
        if (incoming) {
            const IPAddress ip = udp.remoteIP();
            source = address(ip, udp.remotePort());
            tracing_read = true;
            const bool ok = udp.readPacketInto(packet, static_cast<size_t>(incoming));
            tracing_read = false;
            if (!ok) return 2;
        }
        hdt_addr destination;
        size_t outgoing;
        const int step = hdt_r4_bench_step(millis() - start, &source, incoming, &destination, &outgoing);
        if (step != 0) return step > 0 ? 0 : (step == -2 ? 7 : 3);
        if (finding && !hdt_r4_bench_looking()) {
            finding = false;
            report("R4_FOUND", 0);
        }
        if (outgoing) {
            // Closer nodes and relay-supplied destinations are untrusted.
            const bool lan = hdt_r4_bench_lan_destination(&destination) != 0;
            if (!public_address(destination) && !lan) {
                if (finding) continue;
                return 8;
            }
            if (lan && !reported_lan) {
                report("R4_LAN", 0);
                reported_lan = true;
            }
            const IPAddress ip(destination.ip);
            if (outgoing > datagram_max || !udp.beginPacket(ip, destination.port) ||
                udp.write(packet, outgoing) != outgoing || !udp.endPacket()) return 4;
        }
        if (!incoming && !outgoing) delay(1);
    }
    return finding ? 7 : 5;
}

static bool wait_for_address(IPAddress &local) {
    const uint32_t start = millis();
    do {
        local = WiFi.localIP();
        if (local != IPAddress(0, 0, 0, 0)) return true;
        delay(100);
    } while (millis() - start < 5000);
    return false;
}

void setup() {
    const bool painted = paint_stack();
    Serial.begin(115200);
    delay(500);
    if (!painted) { Serial.println(F("R4_FAIL stack_mode")); return; }
    const int startup = hdt_r4_measure_main();
    report("R4_STARTUP", startup);
    if (startup != 0) { Serial.println(F("R4_FAIL startup")); return; }
    while (!Serial.available()) {
        Serial.println(F("R4_READY"));
        delay(500);
    }
    IPAddress local;
    const int status = join();
    report("R4_WIFI", status);
    if (status != WL_CONNECTED || !wait_for_address(local) || !udp.begin(board_port)) {
        WiFi.disconnect();
        Serial.println(F("R4_FAIL join_or_bind"));
        return;
    }
    int result = session(local);
    hdt_r4_bench_clear();
    // A malformed modem reply may have left unread binary bytes on the UART.
    if (result != 2) {
        udp.stop();
        WiFi.disconnect();
    }
    if (!result && (allocations_failed != 0 ||
        stack_used() >= reinterpret_cast<uintptr_t>(&__StackTop) -
                        reinterpret_cast<uintptr_t>(&__StackLimit))) result = 6;
    report("R4_DONE", result);
}

void loop() { delay(250); }
