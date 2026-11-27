/* SPDX-License-Identifier: LGPL-3.0-only
 * New adapter glue for the pinned Arduino/FSP SCE5 interface. See
 * docs/R4-ENTROPY.md; no Renesas driver implementation is copied here. */
#if !defined(ARDUINO_UNOR4_WIFI) && !defined(ARDUINO_UNOR4_MINIMA)
#error "Select an Arduino UNO R4 board explicitly for this entropy provider"
#endif
#include "entropy.h"
#include <bsp_api.h>
#include <sodium.h>
#include <stdint.h>
#include <string.h>

/* Same private API signatures as the pinned core's WMath.cpp. */
extern fsp_err_t HW_SCE_McuSpecificInit(void);
extern fsp_err_t HW_SCE_RNG_Read(uint32_t *out);
static int state; /* 0 uninitialized, 1 ready, -1 hardware failure */

int hdt_ra4m1_entropy(void *out, size_t size) {
    uint8_t *cursor = out;
    size_t remaining = size;
    uint32_t block[4];
    if (!size) return 0;
    if (!out) return -1;
    if (state < 0) goto failed;
    if (!state) {
        if (HW_SCE_McuSpecificInit() != FSP_SUCCESS) {
            state = -1;
            goto failed;
        }
        state = 1;
    }
    while (remaining) {
        if (HW_SCE_RNG_Read(block) != FSP_SUCCESS) {
            state = -1;
            goto failed;
        }
        size_t count = remaining < sizeof(block) ? remaining : sizeof(block);
        memcpy(cursor, block, count);
        sodium_memzero(block, sizeof(block));
        cursor += count;
        remaining -= count;
    }
    return 0;
failed:
    sodium_memzero(block, sizeof(block));
    sodium_memzero(out, size);
    return -1;
}
