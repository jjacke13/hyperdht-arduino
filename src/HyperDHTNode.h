// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <hyperdht/hyperdht.h>

// All methods and native C API calls belong to the setup()/loop() task.
class HyperDHTNode {
public:
    HyperDHTNode() = default;
    ~HyperDHTNode();
    HyperDHTNode(const HyperDHTNode&) = delete;
    HyperDHTNode& operator=(const HyperDHTNode&) = delete;
    bool begin(const uint8_t *seed = nullptr);
    void poll();
    void end(); // Blocking graceful shutdown; never call from a DHT callback.
    bool ready() const;
    hyperdht_t *nativeHandle() const;
    const hyperdht_keypair_t *keypair() const;
private:
    struct State;
    State *state_ = nullptr;
};
