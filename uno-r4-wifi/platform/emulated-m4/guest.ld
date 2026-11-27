/* QEMU AN386 surrogate: 240 KiB code and exactly 32 KiB SRAM. */
ENTRY(Reset_Handler)
MEMORY {
  CODE (rx) : ORIGIN = 0, LENGTH = 240K
  RAM (rw) : ORIGIN = 0x20000000, LENGTH = 32K
}
SECTIONS {
  .vectors : { KEEP(*(.vectors)) } > CODE
  .text : { *(.text*) *(.rodata*) } > CODE
  .ARM.extab : { *(.ARM.extab*) } > CODE
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > CODE
  .data : ALIGN(8) {
    __data_start = .; *(.data*) . = ALIGN(8); __data_end = .;
  } > RAM AT> CODE
  __data_load = LOADADDR(.data);
  __flash_end = LOADADDR(.data) + SIZEOF(.data);
  .bss (NOLOAD) : ALIGN(8) {
    __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(8); __bss_end = .;
  } > RAM
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  __stack_limit = __stack_top - 8192;
  /* MPU denies the whole preceding 8 KiB to catch large crypto stack jumps. */
  .guard 0x20004000 (NOLOAD) : { . += 8192; } > RAM
  .stack __stack_limit (NOLOAD) : { . += 8192; } > RAM
  /DISCARD/ : { *(.comment) *(.note*) }
  ASSERT(__bss_end <= 0x20004000, "static RAM overlaps emulation MPU guard")
  ASSERT(__flash_end <= LENGTH(CODE), "240 KiB code cap exceeded")
}
