// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <stddef.h>
#include <stdint.h>

#if !defined(ARDUINO_UNOR4_WIFI)
#error "HyperDHT Tiny requires an Arduino Uno R4 WiFi and the supplied build profile"
#endif

// One board-wide listener and one active session. Call from setup()/loop().
class HyperDHTServer {
public:
    enum class Status : uint8_t { Idle, Starting, Listening, Connected, RemoteEnd, Stopping, Failed };
    static constexpr size_t MaxRecord = 1024;

    // Join Wi-Fi first. Optional 32-byte secret seed is copied. A null seed
    // creates a fresh identity. Keep the seed private if persisting it.
    static bool begin(const uint8_t *seed = nullptr);
    static Status poll();
    static Status status();
    static const uint8_t *publicKey();
    static bool write(const void *data, size_t size);
    // The view may be invalidated by poll(), release() or stop().
    // Leave the record pending on write backpressure and retry after polling.
    static const uint8_t *read(size_t &size);
    static void release();
    static bool end();
    // Drop the session and start withdrawal. Keep polling until Idle/Failed.
    static void stop();

private:
    HyperDHTServer() = delete;
};
