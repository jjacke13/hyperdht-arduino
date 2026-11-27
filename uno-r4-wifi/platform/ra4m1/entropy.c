/* SPDX-License-Identifier: LGPL-3.0-only */
#include <sodium.h>
#include <stdint.h>

/* Fail closed. No deterministic RNG or fake networking in the target image.
 * Replace with a reviewed board entropy source before attempting execution. */
__attribute__((weak))
int hdt_ra4m1_entropy(void *out, size_t size) {
    (void)out; (void)size;
    return -1;
}
static const char *name(void) { return "unimplemented-ra4m1-entropy"; }
static void fill(void *out, size_t size) {
    if (hdt_ra4m1_entropy(out, size)) {
        sodium_memzero(out, size);
        for (;;) __asm__ volatile ("wfi");
    }
}
static uint32_t word(void) {
    uint32_t value;
    fill(&value, sizeof(value));
    return value;
}
static const randombytes_implementation entropy = { name, word, NULL, NULL, fill, NULL };
void hdt_ra4m1_install_entropy(void) {
    (void)randombytes_set_implementation(&entropy);
}
