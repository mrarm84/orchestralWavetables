#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decodes a raw deflate stream. Returns the number of bytes written,
   or a negative number on error. */
int inflate_raw(uint8_t *dest, uint32_t destlen,
                const uint8_t *source, uint32_t sourcelen);

#ifdef __cplusplus
}
#endif
