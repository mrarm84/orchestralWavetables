/* enosc3 loader.
 *
 * The bootloader jumps here (0x08004000) like it would to a normal
 * application. We inflate the compressed application into SRAM1 at
 * 0x20010000, check it, and jump to it. The application's vector table
 * is the first thing in its image; it sets VTOR itself.
 *
 * Runs with the clocks the bootloader left (HSI 16 MHz), before any
 * .data/.bss exist: no globals here, only the stack.
 */

#include <stdint.h>
#include "stm32f7xx.h"
#include "inflate.h"
#include "image.h"

#define IMAGE_ADDR 0x20010000u
#define IMAGE_MAX  (112u * 1024u)

void loader_main(void);

__attribute__((section(".isr_vector"), used))
const uint32_t loader_vectors[2] = {
  0x20040000u,                    /* initial stack: top of RAM */
  (uint32_t)&loader_main,         /* reset (thumb bit set by the linker) */
};

__attribute__((noreturn)) static void halt(void) {
  /* corrupt image: stay here rather than run garbage. Re-flash with the
     audio bootloader. */
  while (1) { __NOP(); }
}

__attribute__((noreturn, used)) void loader_main(void) {
  uint8_t *dest = (uint8_t *)IMAGE_ADDR;

  /* FPU on (the application's SystemInit does it too), in case the
     compiler uses FPU registers here */
  SCB->CPACR |= (0xFu << 20);
  __DSB();
  __ISB();

  int n = inflate_raw(dest, IMAGE_MAX, enosc3_blob, enosc3_blob_size);
  if (n != (int)enosc3_image_size) halt();
  if (image_hash(dest, (uint32_t)n) != enosc3_image_hash) halt();

  /* make the written code visible to instruction fetches */
  if (SCB->CCR & SCB_CCR_DC_Msk) SCB_CleanDCache();
  __DSB();
  if (SCB->CCR & SCB_CCR_IC_Msk) SCB_InvalidateICache();
  __DSB();
  __ISB();

  const uint32_t *app = (const uint32_t *)IMAGE_ADDR;
  uint32_t sp = app[0];
  uint32_t pc = app[1];
  __asm volatile (
    "msr msp, %0\n\t"
    "bx %1\n\t"
    : : "r" (sp), "r" (pc) : "memory");
  __builtin_unreachable();
}
