/* SPDX-License-Identifier: LGPL-3.0-only */
#include "hdtiny/noise.h"
#include "hdtiny/record.h"
#include "hyperdht/noise_wrap.hpp"
#include "hyperdht/secret_stream.hpp"
#include <sodium.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace noise = hyperdht::noise;
static noise::Seed seed(unsigned start) {
    noise::Seed s{};
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<uint8_t>(start + i);
    return s;
}
static std::array<uint8_t, 32> prologue() {
    std::array<uint8_t, 33> input{};
    std::array<uint8_t, 32> result{};
    const char name[] = "hyperswarm/dht";
    crypto_generichash(input.data(), 32, reinterpret_cast<const uint8_t *>(name),
                       sizeof(name) - 1, nullptr, 0);
    crypto_generichash(result.data(), 32, input.data(), input.size(), nullptr, 0);
    return result;
}
static void records(hdt_noise &tiny, const noise::NoiseIK &peer, bool initiator) {
    hdt_record record;
    uint8_t header[59], wire[1044], plain[1024];
    size_t used = 0;
    std::memset(plain, 0x6a, sizeof(plain));
    hyperdht::secret_stream::SecretStream original(peer.tx_key(), peer.rx_key(),
                                                    peer.handshake_hash(), !initiator);
    assert(hdt_record_init(&record, tiny.tx, tiny.rx, tiny.hash, initiator, header));
    assert(original.receive_header(header + 3, 56));
    auto theirs = original.create_header_message();
    assert(hdt_record_feed(&record, theirs.data(), theirs.size(), &used) == HDT_RECORD_INPUT);
    assert(used == theirs.size());
    size_t n = hdt_record_seal(&record, plain, sizeof(plain), wire, sizeof(wire));
    assert(n == sizeof(wire));
    auto decoded = original.decrypt(wire + 3, n - 3);
    assert(decoded && decoded->size() == sizeof(plain));
    assert(!std::memcmp(decoded->data(), plain, sizeof(plain)));
    auto encrypted = original.encrypt(plain, sizeof(plain));
    assert(hdt_record_feed(&record, encrypted.data(), encrypted.size(), &used) == HDT_RECORD_MESSAGE);
    assert(used == encrypted.size() && record.plain_size == sizeof(plain));
    assert(!std::memcmp(record.plain, plain, sizeof(plain)));
    hdt_record_clear(&record);
}

static void interoperate(bool initiator, size_t size) {
    auto a = seed(1), b = seed(51), ea = seed(101), eb = seed(151);
    auto ka = noise::generate_keypair(a), kb = noise::generate_keypair(b);
    auto p = prologue();
    noise::NoiseIK first(true, ka, p.data(), p.size(), &kb.public_key);
    noise::NoiseIK second(false, kb, p.data(), p.size());
    first.set_ephemeral(noise::generate_keypair(ea));
    second.set_ephemeral(noise::generate_keypair(eb));
    hdt_noise tiny;
    assert(hdt_noise_init(&tiny, initiator, (initiator ? a : b).data(),
                          (initiator ? ea : eb).data(), initiator ? kb.public_key.data() : nullptr));
    uint8_t payload[512], wire[608], decoded[512];
    size_t written = 0;
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = static_cast<uint8_t>(i);
    auto msg1 = first.send(payload, size);
    if (initiator) {
        assert(hdt_noise_send(&tiny, payload, size, wire, sizeof(wire), &written));
        assert(written == msg1.size() && !std::memcmp(wire, msg1.data(), written));
    } else {
        assert(hdt_noise_receive(&tiny, msg1.data(), msg1.size(), decoded, sizeof(decoded), &written));
        assert(written == size && !std::memcmp(decoded, payload, size));
    }
    auto decoded1 = second.recv(msg1.data(), msg1.size());
    assert(decoded1 && decoded1->size() == size);
    auto msg2 = second.send(payload, size);
    if (initiator) {
        assert(hdt_noise_receive(&tiny, msg2.data(), msg2.size(), decoded, sizeof(decoded), &written));
        assert(written == size && !std::memcmp(decoded, payload, size));
    } else {
        assert(hdt_noise_send(&tiny, payload, size, wire, sizeof(wire), &written));
        assert(written == msg2.size() && !std::memcmp(wire, msg2.data(), written));
    }
    auto decoded2 = first.recv(msg2.data(), msg2.size());
    assert(decoded2 && decoded2->size() == size);
    const noise::NoiseIK &same = initiator ? first : second;
    const noise::NoiseIK &peer = initiator ? second : first;
    assert(hdt_noise_complete(&tiny));
    assert(!std::memcmp(tiny.tx, same.tx_key().data(), 32));
    assert(!std::memcmp(tiny.rx, same.rx_key().data(), 32));
    assert(!std::memcmp(tiny.hash, same.handshake_hash().data(), 64));
    assert(!std::memcmp(tiny.remote, same.remote_public_key().data(), 32));
    assert(sodium_is_zero(tiny.secret, sizeof(tiny.secret)));
    assert(sodium_is_zero(tiny.ephemeral_secret, sizeof(tiny.ephemeral_secret)));
    records(tiny, peer, initiator);
    hdt_noise_clear(&tiny);
    assert(!hdt_noise_complete(&tiny));
}

static void invalid() {
    auto a = seed(1), b = seed(51), ea = seed(101), eb = seed(151);
    auto ka = noise::generate_keypair(a), kb = noise::generate_keypair(b);
    auto p = prologue();
    noise::NoiseIK first(true, ka, p.data(), p.size(), &kb.public_key);
    first.set_ephemeral(noise::generate_keypair(ea));
    auto message = first.send(reinterpret_cast<const uint8_t *>("test"), 4);
    uint8_t out[608]; size_t written;
    hdt_noise n;
    assert(hdt_noise_init(&n, 0, b.data(), eb.data(), nullptr));
    hdt_noise before = n;
    assert(!hdt_noise_send(&n, nullptr, 0, out, sizeof(out), &written));
    assert(!std::memcmp(&n, &before, sizeof(n)));
    assert(!hdt_noise_receive(&n, message.data(), message.size(), out, 3, &written));
    assert(!std::memcmp(&n, &before, sizeof(n)));
    for (size_t i = 0; i < message.size(); ++i) {
        assert(hdt_noise_init(&n, 0, b.data(), eb.data(), nullptr));
        message[i] ^= 1;
        assert(!hdt_noise_receive(&n, message.data(), message.size(), out, sizeof(out), &written));
        assert(n.phase == 255 && !written);
        assert(sodium_is_zero(n.secret, sizeof(n.secret)));
        assert(sodium_is_zero(n.tx, sizeof(n.tx)));
        assert(sodium_is_zero(out, 4));
        message[i] ^= 1;
    }
    for (size_t size = 0; size < 96; ++size) {
        assert(hdt_noise_init(&n, 0, b.data(), eb.data(), nullptr));
        assert(!hdt_noise_receive(&n, message.data(), size, out, sizeof(out), &written));
        assert(n.phase == 255);
    }
    noise::NoiseIK second(false, kb, p.data(), p.size());
    second.set_ephemeral(noise::generate_keypair(eb));
    assert(second.recv(message.data(), message.size()));
    auto reply = second.send(reinterpret_cast<const uint8_t *>("test"), 4);
    uint8_t request[608];
    for (size_t i = 0; i < reply.size(); ++i) {
        assert(hdt_noise_init(&n, 1, a.data(), ea.data(), kb.public_key.data()));
        assert(hdt_noise_send(&n, reinterpret_cast<const uint8_t *>("test"), 4,
                              request, sizeof(request), &written));
        reply[i] ^= 1;
        assert(!hdt_noise_receive(&n, reply.data(), reply.size(), out, sizeof(out), &written));
        assert(n.phase == 255 && !written && sodium_is_zero(out, 4));
        assert(sodium_is_zero(n.ephemeral_secret, sizeof(n.ephemeral_secret)));
        reply[i] ^= 1;
    }
    assert(hdt_noise_init(&n, 1, a.data(), ea.data(), kb.public_key.data()));
    before = n;
    assert(!hdt_noise_send(&n, out, 513, out, sizeof(out), &written));
    assert(!std::memcmp(&n, &before, sizeof(n)));
    assert(!hdt_noise_send(&n, nullptr, 0, out, 95, &written));
    assert(!std::memcmp(&n, &before, sizeof(n)));
    uint8_t zero[32] = {0};
    assert(hdt_noise_init(&n, 1, a.data(), ea.data(), zero));
    assert(!hdt_noise_send(&n, nullptr, 0, out, sizeof(out), &written));
    assert(n.phase == 255);
    /* The connector must fail when its selected responder identity is wrong. */
    assert(hdt_noise_init(&n, 0, a.data(), eb.data(), nullptr));
    assert(!hdt_noise_receive(&n, message.data(), message.size(), out, sizeof(out), &written));
    assert(n.phase == 255);
}
int main() {
    assert(sodium_init() >= 0);
    for (bool initiator : {false, true})
        for (size_t size : {size_t(0), size_t(1), size_t(512)}) interoperate(initiator, size);
    invalid();
    std::printf("Noise IK original peer parity + authenticated records: PASS; state=%zu\n", sizeof(hdt_noise));
}
