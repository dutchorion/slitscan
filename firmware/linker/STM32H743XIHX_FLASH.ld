/* Linker script for STM32H743XIHx: 2 MB flash, 1 MB RAM, 32 MB external SDRAM */

ENTRY(Reset_Handler)

_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);   /* stack at top of DTCM */
_Min_Heap_Size  = 0x2000;
_Min_Stack_Size = 0x4000;

MEMORY
{
  FLASH   (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  DTCMRAM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K   /* CPU only, no DMA */
  RAM_D1  (xrw) : ORIGIN = 0x24000000, LENGTH = 512K   /* AXI SRAM */
  RAM_D2  (xrw) : ORIGIN = 0x30000000, LENGTH = 288K   /* DMA buffers */
  RAM_D3  (xrw) : ORIGIN = 0x38000000, LENGTH = 64K    /* BDMA buffers */
  ITCMRAM (xrw) : ORIGIN = 0x00000000, LENGTH = 64K
  SDRAM   (rw)  : ORIGIN = 0xC0000000, LENGTH = 32M
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*) *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP (*(.init)) KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata) *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH

  .preinit_array : { PROVIDE_HIDDEN (__preinit_array_start = .); KEEP (*(.preinit_array*)) PROVIDE_HIDDEN (__preinit_array_end = .); } >FLASH
  .init_array    : { PROVIDE_HIDDEN (__init_array_start = .); KEEP (*(SORT(.init_array.*))) KEEP (*(.init_array*)) PROVIDE_HIDDEN (__init_array_end = .); } >FLASH
  .fini_array    : { PROVIDE_HIDDEN (__fini_array_start = .); KEEP (*(SORT(.fini_array.*))) KEEP (*(.fini_array*)) PROVIDE_HIDDEN (__fini_array_end = .); } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data) *(.data*) *(.RamFunc) *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM_D1 AT> FLASH

  .bss (NOLOAD) :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM_D1

  ._user_heap :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = ALIGN(8);
  } >RAM_D1

  ._user_stack :
  {
    . = ALIGN(8);
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >DTCMRAM

  /* DMA-reachable buffers (not zeroed at startup) */
  .dma_buffer (NOLOAD) : { . = ALIGN(32); *(.dma_buffer) *(.dma_buffer*) } >RAM_D2
  .bdma_buffer (NOLOAD) : { . = ALIGN(32); *(.bdma_buffer) *(.bdma_buffer*) } >RAM_D3
  .sdram (NOLOAD) : { . = ALIGN(32); *(.sdram) *(.sdram*) } >SDRAM

  /DISCARD/ : { libc.a ( * ) libm.a ( * ) libgcc.a ( * ) }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
