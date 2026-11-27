// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

#if !defined(ARDUINO_UNOR4_WIFI)
#error "HyperDHT Tiny requires an Arduino Uno R4 WiFi and the supplied build profile"
#endif

// One board-wide outbound session. Call from setup()/loop() only.
class HyperDHTClient {
public:
    enum class Status : uint8_t { Idle, Connecting, Connected, RemoteEnd, Finished, Failed };
    static constexpr size_t MaxRecord = 1024;

    // Join Wi-Fi first. begin() resolves bootstraps and binds a UDP socket.
    // Supply the remote public key as 32 raw bytes. The key is copied.
    static bool begin(const uint8_t publicKey[32]);
    static Status poll();
    static Status status();
    // One record per write. Returns false on backpressure or invalid length.
    static bool write(const void *data, size_t size);
    // Borrow authenticated data. Release it before polling again.
    static const uint8_t *read(size_t &size);
    static void release();
    // Queue a graceful write-side close. Retry while false and keep polling.
    static bool end();
    // Drop the session and release its socket. Wi-Fi stays connected.
    static void close();

private:
    HyperDHTClient() = delete;
};
