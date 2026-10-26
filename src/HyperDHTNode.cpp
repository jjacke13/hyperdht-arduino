// SPDX-License-Identifier: AGPL-3.0-or-later
#include "HyperDHTNode.h"
#include <uv.h>
#include <cstring>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct HyperDHTNode::State {
    uv_loop_t loop{};
    hyperdht_t *dht = nullptr;
    hyperdht_keypair_t identity{};
    TaskHandle_t owner = xTaskGetCurrentTaskHandle();
    bool polling = false;
};

HyperDHTNode::~HyperDHTNode() { end(); }

bool HyperDHTNode::begin(const uint8_t *seed) {
    if (state_) return false;
    State *s = new(std::nothrow) State;
    if (!s) return false;
    if (uv_loop_init(&s->loop) < 0) { delete s; return false; }
    state_ = s;
    hyperdht_opts_t options;
    hyperdht_opts_default(&options);
    options.use_public_bootstrap = 1;
    if (seed) {
        options.seed_is_set = 1;
        std::memcpy(options.seed, seed, sizeof(options.seed));
    }
    s->dht = hyperdht_create(&s->loop, &options);
    // Explicit zeroing routine is not optimized away.
    volatile uint8_t *wipe = options.seed;
    for (size_t i = 0; i < sizeof(options.seed); ++i) wipe[i] = 0;
    if (!s->dht) { uv_loop_close(&s->loop); delete s; state_ = nullptr; return false; }
    hyperdht_default_keypair(s->dht, &s->identity);
    if (hyperdht_bind(s->dht, 0) < 0) { end(); return false; }
    return true;
}

void HyperDHTNode::poll() {
    if (!state_) return;
    configASSERT(state_->owner == xTaskGetCurrentTaskHandle() && !state_->polling);
    state_->polling = true;
    uv_run(&state_->loop, UV_RUN_NOWAIT);
    state_->polling = false;
}

void HyperDHTNode::end() {
    if (!state_) return;
    State *s = state_;
    configASSERT(s->owner == xTaskGetCurrentTaskHandle() && !s->polling);
    hyperdht_destroy(s->dht, nullptr, nullptr);
    uv_run(&s->loop, UV_RUN_DEFAULT);
    hyperdht_free(s->dht);
    hyperdht_keypair_zero(&s->identity);
    const int rc = uv_loop_close(&s->loop);
    configASSERT(rc == 0);
    delete s;
    state_ = nullptr;
}

bool HyperDHTNode::ready() const { return state_ && hyperdht_is_bootstrapped(state_->dht); }
hyperdht_t *HyperDHTNode::nativeHandle() const { return state_ ? state_->dht : nullptr; }
const hyperdht_keypair_t *HyperDHTNode::keypair() const { return state_ ? &state_->identity : nullptr; }
