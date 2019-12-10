#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t crc32(uint32_t crc, const void *ptr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // CRC32_H
