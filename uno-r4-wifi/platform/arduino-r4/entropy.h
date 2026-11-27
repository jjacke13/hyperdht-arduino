/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDT_ARDUINO_R4_ENTROPY_H
#define HDT_ARDUINO_R4_ENTROPY_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Explicit opt-in provider for the existing RA4M1 platform hook. Serialized,
 * sole SCE owner; call from main context only. Zero on success, -1 on failure.
 * Hardware failure wipes the complete output and latches until device reset.
 * Zero length succeeds without hardware access; NULL with nonzero size fails. */
int hdt_ra4m1_entropy(void *out, size_t size);
/* Existing platform/ra4m1/entropy.c must also be linked. Install before sodium
 * initialization; its callback wipes and halts if the provider reports failure. */
void hdt_ra4m1_install_entropy(void);
#ifdef __cplusplus
}
#endif
#endif
