/*
 * enosc3: the application runs from RAM.
 *
 * The 48K of internal flash after the bootloader holds a small loader
 * (loader/) and this application, deflate-compressed. At boot the
 * loader inflates the image to IMAGE (SRAM1) and jumps to it. The image
 * starts with the vector table, like a flash build.
 *
 * RAM map (256K):
 *   0x20000000  DTCM   64K  .bss (DMA buffers must stay here: not cached)
 *   0x20010000  IMAGE 112K  vectors, code, rodata, data, ITCM code copy
 *   0x2002C000  SRAM   80K  .sram_noinit tables, heap, stack (top)
 */

ENTRY(Reset_Handler)

_estack = 0x20040000;
_Min_Heap_Size = 0x400;
_Min_Stack_Size = 0x1000;

_image_start = 0x20010000;

MEMORY
{
ITCM_RAM (rx)  : ORIGIN = 0x00000000, LENGTH = 16K
DTCM (xrw)     : ORIGIN = 0x20000000, LENGTH = 64K
IMAGE (xrw)    : ORIGIN = 0x20010000, LENGTH = 112K
SRAM (xrw)     : ORIGIN = 0x2002C000, LENGTH = 80K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >IMAGE

  ASSERT(ADDR(.isr_vector) == _image_start, "vector table must start the image")

  _siitcm = LOADADDR(.itcm_text);
  .itcm_text :
  {
    . = ALIGN(4);
    _sitcm = .;
    *(.itcm_text)
    *(.itcm_text*)
    *(*__IN_ITCM_*)
    . = ALIGN(4);
    _eitcm = .;
  } >ITCM_RAM AT> IMAGE

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;
  } >IMAGE

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >IMAGE

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >IMAGE
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >IMAGE

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >IMAGE
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >IMAGE
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >IMAGE

  /* .data runs where it is loaded: the startup code copies it onto
     itself, which is harmless */
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >IMAGE

  _image_end = .;

  .bss (NOLOAD) :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >DTCM

  /* Large CPU-only arrays in cached SRAM. Not zeroed at startup: their
     owners initialize them. */
  .sram_noinit (NOLOAD) :
  {
    . = ALIGN(4);
    *(.sram_noinit*)
    . = ALIGN(4);
  } >SRAM

  ._user_heap_stack (NOLOAD) :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >SRAM

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
