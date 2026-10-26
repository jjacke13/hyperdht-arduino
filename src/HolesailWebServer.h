// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "HyperDHTNode.h"

// Public-mode HTTP server. Anyone knowing the public link may connect.
class HolesailWebServer {
public:
    HolesailWebServer() = default;
    ~HolesailWebServer();
    HolesailWebServer(const HolesailWebServer&) = delete;
    HolesailWebServer& operator=(const HolesailWebServer&) = delete;
    // page must remain valid and unchanged until end(). seed is copied at begin.
    bool begin(const char *page, const uint8_t *seed = nullptr);
    void poll();
    void end(); // Call only from setup()/loop(), outside callbacks.
    bool ready() const;
    const char *link() const;
private:
    struct State;
    State *state_ = nullptr;
};
