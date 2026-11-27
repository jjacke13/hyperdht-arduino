/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef HDT_R4_BENCH_SESSION_H
#define HDT_R4_BENCH_SESSION_H
#include "hdtiny/rpc.h"
#ifdef __cplusplus
extern "C" {
#endif
uint8_t *hdt_r4_bench_packet(void);
int hdt_r4_measure_main(void);
int hdt_r4_bench_begin(const uint8_t key[32], const hdt_addr *seeds, size_t count);
int hdt_r4_bench_looking(void);
void hdt_r4_bench_set_lan(const hdt_addr *local, const uint8_t mask[4]);
int hdt_r4_bench_lan_destination(const hdt_addr *to);
/* Source/length describe RX already in the scratch packet. TX, if any,
 * overwrites scratch. Return -2 on lookup failure, -1 on session failure, 0 while active, 1 on EOF. */
int hdt_r4_bench_step(uint64_t now, const hdt_addr *source, size_t received,
                       hdt_addr *destination, size_t *outgoing);
void hdt_r4_bench_clear(void);
#ifdef __cplusplus
}
#endif
#endif
