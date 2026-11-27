/* SPDX-License-Identifier: LGPL-3.0-only */
.syntax unified
.cpu cortex-m4
.thumb
.section .vectors,"a",%progbits
.word __stack_top
.word Reset_Handler
.rept 14
.word fault_halt
.endr
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.thumb_func
Reset_Handler:
  ldr r0, =__data_load
  ldr r1, =__data_start
  ldr r2, =__data_end
1: cmp r1, r2
  bhs 2f
  ldr r3, [r0], #4
  str r3, [r1], #4
  b 1b
2: ldr r1, =__bss_start
  ldr r2, =__bss_end
  movs r3, #0
3: cmp r1, r2
  bhs 4f
  str r3, [r1], #4
  b 3b
4: ldr r1, =__stack_limit
  ldr r2, =__stack_top
  ldr r3, =0xa5a5a5a5
5: cmp r1, r2
  bhs 6f
  str r3, [r1], #4
  b 5b
6: ldr r1, =emulated_min_msp
  str r2, [r1]
  bl emulated_main
.global fault_halt
.thumb_func
fault_halt:
  wfi
  b fault_halt
/* No hook stack frame. Function-entry samples include the callee's prologue. */
.global __cyg_profile_func_enter
.thumb_func
__cyg_profile_func_enter:
  mrs r2, msp
  ldr r3, =emulated_min_msp
  ldr r0, [r3]
  cmp r2, r0
  bhs 7f
  str r2, [r3]
7: bx lr
.global __cyg_profile_func_exit
.thumb_func
__cyg_profile_func_exit:
  bx lr
