/* SPDX-License-Identifier: LGPL-3.0-only
 * Test-only QEMU guest. Deterministic entropy MUST NOT reach board firmware. */
#ifndef HDT_EMULATED_M4_TEST_ONLY
#error "This guest and its deterministic RNG are exclusively for emulation tests"
#endif
#include "hdtiny/connector.h"
#include <stdint.h>
#include <string.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define UART 0x40004000u
extern uint32_t __stack_limit, __stack_top;
uint32_t emulated_min_msp;
static hdt_connector connector;
static uint8_t frame[2063];
static uint64_t previous_time;
static uint32_t rng_state = 0x64c0ffee;
static uint8_t configured, sent, echoed, ending;

static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void store32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8*i));
}
static uint8_t uart_get(void) {
    while (!(REG(UART+4) & 2)) {}
    return (uint8_t)REG(UART);
}
static void uart_put(uint8_t byte) {
    while (REG(UART+4) & 1) {}
    REG(UART) = byte;
}
static void output(size_t size) {
    uart_put((uint8_t)size); uart_put((uint8_t)(size >> 8));
    for (size_t i = 0; i < size; ++i) uart_put(frame[i]);
}
__attribute__((noreturn))
static void halt(void) { for (;;) __asm__ volatile ("wfi"); }
static void sample_msp(void) {
    uint32_t msp;
    __asm__ volatile ("mrs %0, msp" : "=r" (msp));
    if (msp < emulated_min_msp) emulated_min_msp = msp;
}
__attribute__((noreturn))
static void result(uint32_t status) {
    sample_msp();
    uintptr_t low = (uintptr_t)&__stack_limit, top = (uintptr_t)&__stack_top;
    volatile const uint32_t *p = (const uint32_t *)low;
    while ((uintptr_t)p < top && *p == UINT32_C(0xa5a5a5a5)) ++p;
    uint32_t touched = (uint32_t)(top - (uintptr_t)p);
    if (emulated_min_msp < low || emulated_min_msp > top) status = 7;
    uint32_t sampled = (uint32_t)(top - emulated_min_msp);
    frame[0] = 4; store32(frame+1, status);
    store32(frame+5, touched > sampled ? touched : sampled);
    store32(frame+9, emulated_min_msp);
    output(13);
    while (REG(UART+4) & 1) {}
    /* The orchestrator owns QEMU shutdown after receiving this terminal frame. */
    halt();
}
__attribute__((noreturn)) void abort(void) { result(8); }
__attribute__((noreturn))
void __assert_func(const char *file, int line, const char *function, const char *test) {
    (void)file; (void)line; (void)function; (void)test;
    result(8);
}
static uint32_t random_word(void) {
    uint32_t n = rng_state;
    n ^= n << 13; n ^= n >> 17; n ^= n << 5;
    return rng_state = n;
}
static void random_fill(void *out, size_t size) {
    uint8_t *p = out;
    while (size--) *p++ = (uint8_t)random_word();
}
static const char *random_name(void) { return "INSECURE-emulator-test-only"; }
static const randombytes_implementation test_rng = {
    random_name, random_word, NULL, NULL, random_fill, NULL
};
static void region(uint32_t index, uint32_t base, uint32_t size_power, uint32_t access, uint32_t xn) {
    REG(0xe000ed98) = index;
    REG(0xe000ed9c) = base;
    uint32_t memory = (index == 0 || index == 2) ? 1u << 19 : 0; /* Normal, uncached code/RAM. */
    REG(0xe000eda0) = (xn << 28) | (access << 24) | memory | ((size_power-1) << 1) | 1;
}
static void restrict_memory(void) {
    if (((REG(0xe000ed90) >> 8) & 255) < 6) halt();
    REG(0xe000ed94) = 0;
    region(0, 0, 18, 6, 0);             /* Read-only executable 256 KiB. */
    region(1, 0x0003c000, 14, 0, 1);    /* Deny final 16 KiB: 240 KiB cap. */
    region(2, 0x20000000, 15, 3, 1);    /* Only 32 KiB SRAM. */
    region(3, UART, 12, 3, 1);          /* UART0 only, no Ethernet MMIO. */
    region(4, 0xe0000000, 20, 3, 1);    /* CPU system/debug registers. */
    region(5, 0x20004000, 13, 0, 1);    /* 8 KiB guard below the 8 KiB stack. */
    REG(0xe000ed24) |= 1u << 16;
    REG(0xe000ed94) = 1;                /* No privileged background region. */
    __asm__ volatile ("dsb\nisb" ::: "memory");
}
static void configure(size_t size) {
    if (configured || size != 39) result(2);
    hdt_addr peer;
    uint8_t seed[32], ephemeral[32];
    memcpy(peer.ip, frame+1, 4); peer.port = (uint16_t)(frame[5] | (uint16_t)frame[6] << 8);
    randombytes_set_implementation(&test_rng);
    if (sodium_init() < 0) result(3);
    randombytes_buf(seed, sizeof(seed)); randombytes_buf(ephemeral, sizeof(ephemeral));
    int ok = hdt_connector_begin(&connector, &peer, frame+7, seed, ephemeral,
                                 randombytes_random(), (uint16_t)randombytes_random());
    sodium_memzero(seed, sizeof(seed)); sodium_memzero(ephemeral, sizeof(ephemeral));
    if (!ok) result(3);
    configured = 1;
    memset(frame, 0, 7); frame[0] = 3; output(7);
}
static void step(size_t size) {
    if (!configured || size < 15 || size > 2063) result(2);
    uint64_t now = (uint64_t)load32(frame+1) | (uint64_t)load32(frame+5) << 32;
    if (now < previous_time) result(6);
    previous_time = now;
    if (size > 15) {
        hdt_addr source;
        memcpy(source.ip, frame+9, 4); source.port = (uint16_t)(frame[13] | (uint16_t)frame[14] << 8);
        /* Foreign/duplicate datagrams may validly be ignored by the core. */
        (void)hdt_connector_receive(&connector, &source, frame+15, size-15);
    }
    if (connector.status == HDT_CONNECT_FAILED) result(4);
    if (!sent && connector.status == HDT_CONNECT_READY) {
        for (size_t i = 0; i < 1024; ++i) frame[i] = (uint8_t)i;
        sent = (uint8_t)hdt_connector_write(&connector, frame, 1024);
    }
    size_t plain_size;
    const uint8_t *plain = hdt_connector_read(&connector, &plain_size);
    if (plain) {
        if (echoed || plain_size != 1024) result(5);
        for (size_t i = 0; i < plain_size; ++i) if (plain[i] != (uint8_t)i) result(5);
        echoed = 1; hdt_connector_release(&connector);
    }
    if (sent && echoed && !ending) ending = (uint8_t)hdt_connector_end(&connector);
    hdt_addr destination;
    size_t n = hdt_connector_poll(&connector, now, &destination, frame+7, sizeof(frame)-7);
    if (connector.status == HDT_CONNECT_FAILED) result(4);
    sample_msp();
    /* Offer any final ACK to the host before declaring success on a later tick. */
    if (!n && sent && echoed && ending && connector.status == HDT_CONNECT_EOF &&
        connector.state.stream.transport.local_end && !connector.state.stream.transport.queued) result(0);
    frame[0] = 3;
    if (n) {
        memcpy(frame+1, destination.ip, 4); frame[5] = (uint8_t)destination.port;
        frame[6] = (uint8_t)(destination.port >> 8);
    } else memset(frame+1, 0, 6);
    output(n+7);
}
void emulated_main(void) {
    REG(UART+0x10) = 217; REG(UART+8) = 3;
    /* QEMU re-arms chardev input on DATA reads, not on the RX-enable write. */
    if (!(REG(UART+4) & 2)) (void)REG(UART);
    restrict_memory();
    for (;;) {
        size_t size = uart_get(); size |= (size_t)uart_get() << 8;
        if (!size || size > sizeof(frame)) result(1);
        for (size_t i = 0; i < size; ++i) frame[i] = uart_get();
        if (frame[0] == 1) configure(size);
        else if (frame[0] == 2) step(size);
        else result(1);
    }
}
