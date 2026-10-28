/* CRC-32C (Castagnoli, reflected polynomial 0x82F63B78), software slice-by-8. */
#ifndef NVMETAL_CRC32C_H
#define NVMETAL_CRC32C_H

#include <stddef.h>
#include <stdint.h>

/* crc32c(0, data, len) gives the standard CRC-32C; pass a previous result to continue. */
uint32_t crc32c(uint32_t crc, const void *data, size_t len);

#endif
