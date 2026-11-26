/* enosc3 loader: lives in the application flash area after the
   bootloader, with the compressed application image. */

ENTRY(loader_main)

MEMORY
{
FLASH (rx) : ORIGIN = 0x08004000, LENGTH = 48K
}

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* the loader runs before anything initializes RAM */
  .data : { *(.data) *(.data*) } >FLASH
  ASSERT(SIZEOF(.data) == 0, "loader must not have initialized data")
  .bss : { *(.bss) *(.bss*) *(COMMON) } >FLASH
  ASSERT(SIZEOF(.bss) == 0, "loader must not have zero-initialized data")

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
