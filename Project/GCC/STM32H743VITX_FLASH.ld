ENTRY(Reset_Handler)

_Min_Heap_Size = 0x2000;
_Min_Stack_Size = 0x2000;

MEMORY
{
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D1  (xrw) : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 124K
  RAM_D3  (xrw) : ORIGIN = 0x38000000, LENGTH = 64K
}

_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(1024);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } > FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text .text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP(*(.init))
    KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata .rodata*)
    . = ALIGN(4);
  } > FLASH

  .ARM.extab :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } > FLASH

  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } > FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } > FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } > FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data .data*)
    *(.RamFunc .RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } > DTCMRAM AT> FLASH

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss .bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } > DTCMRAM

  .dtcm_ram (NOLOAD) :
  {
    . = ALIGN(4);
    *(.dtcm_ram .dtcm_ram*)
    . = ALIGN(4);
  } > DTCMRAM

  .axi_sram (NOLOAD) :
  {
    . = ALIGN(4);
    *(.axi_sram .axi_sram*)
    . = ALIGN(4);
  } > RAM_D1

  .sram (NOLOAD) :
  {
    . = ALIGN(4);
    *(.sram .sram*)
    . = ALIGN(4);
  } > RAM_D2

  .backup_sram (NOLOAD) :
  {
    . = ALIGN(4);
    *(.backup_sram .backup_sram*)
    . = ALIGN(4);
  } > RAM_D3

  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . += _Min_Heap_Size;
    . += _Min_Stack_Size;
    . = ALIGN(8);
  } > DTCMRAM
}
