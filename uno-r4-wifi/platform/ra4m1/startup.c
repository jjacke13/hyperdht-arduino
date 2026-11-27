/* SPDX-License-Identifier: LGPL-3.0-only
 * Measurement startup, deliberately NOT an RA4M1 board support package. */
#include <stdint.h>
extern uint32_t __data_load, __data_start, __data_end, __bss_start, __bss_end;
extern uint32_t __stack_top;
int main(void);
__attribute__((noreturn))
static void halt(void) { for (;;) __asm__ volatile ("wfi"); }
/* Bare-metal fatal paths retain validation without pulling stdio/heap/signals. */
__attribute__((noreturn))
void abort(void) { halt(); }
__attribute__((noreturn))
void __assert_func(const char *file, int line, const char *function, const char *test) {
    (void)file; (void)line; (void)function; (void)test;
    halt();
}
void Reset_Handler(void) {
    uint32_t *src = &__data_load;
    for (uint32_t *p = &__data_start; p < &__data_end; ++p) *p = *src++;
    for (uint32_t *p = &__bss_start; p < &__bss_end; ++p) *p = 0;
    (void)main();
    halt();
}
typedef void (*vector)(void);
__attribute__((used, section(".vectors")))
const vector vectors[16] = {
    (vector)&__stack_top, Reset_Handler, halt, halt, halt, halt, halt,
    0, 0, 0, 0, halt, halt, 0, halt, halt
};
