/* Link measurement only: UNO R4 application origin, excluding its 16 KiB loader.
 * No option-setting bytes, BSP, clocks, interrupt routing or board drivers. */
ENTRY(Reset_Handler)
MEMORY {
  FLASH (rx) : ORIGIN = 0x00004000, LENGTH = 240K
  RAM (rw) : ORIGIN = 0x20000000, LENGTH = 32K
}
SECTIONS {
  .vectors : { KEEP(*(.vectors)) } > FLASH
  .text : { *(.text*) *(.rodata*) } > FLASH
  .ARM.extab : { *(.ARM.extab*) } > FLASH
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
  .data : ALIGN(8) {
    __data_start = .; *(.data*) . = ALIGN(8); __data_end = .;
  } > RAM AT> FLASH
  __data_load = LOADADDR(.data);
  __flash_end = LOADADDR(.data) + SIZEOF(.data);
  .bss (NOLOAD) : ALIGN(8) {
    __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(8); __bss_end = .;
  } > RAM
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  __stack_limit = __stack_top - 8192;
  .stack __stack_limit (NOLOAD) : { . += 8192; } > RAM
  /DISCARD/ : { *(.comment) *(.note*) }
  ASSERT(__bss_end <= __stack_limit, "static RAM overlaps reserved 8 KiB stack")
  ASSERT(__flash_end <= ORIGIN(FLASH) + LENGTH(FLASH), "application flash overflow")
}
