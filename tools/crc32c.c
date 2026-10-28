#include "crc32c.h"

#include <pthread.h>
#include <string.h>

static uint32_t table[8][256];
static pthread_once_t table_once = PTHREAD_ONCE_INIT;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0x82f63b78u & (0u - (c & 1)));
        table[0][i] = c;
    }
    /* table[k][i]: CRC of byte i followed by k zero bytes, so eight table
     * lookups advance the CRC over eight input bytes at once. */
    for (uint32_t i = 0; i < 256; i++)
        for (int k = 1; k < 8; k++)
            table[k][i] = (table[k - 1][i] >> 8) ^ table[0][table[k - 1][i] & 0xff];
}

uint32_t crc32c(uint32_t crc, const void *data, size_t len)
{
    pthread_once(&table_once, build_table);
    const uint8_t *p = data;
    crc = ~crc;
    while (len && ((uintptr_t)p & 7)) {
        crc = (crc >> 8) ^ table[0][(crc ^ *p++) & 0xff];
        len--;
    }
    while (len >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        w ^= crc; /* little endian: the CRC folds into the low four bytes */
        crc = table[7][w & 0xff] ^ table[6][(w >> 8) & 0xff] ^ table[5][(w >> 16) & 0xff] ^
              table[4][(w >> 24) & 0xff] ^ table[3][(w >> 32) & 0xff] ^ table[2][(w >> 40) & 0xff] ^
              table[1][(w >> 48) & 0xff] ^ table[0][w >> 56];
        p += 8;
        len -= 8;
    }
    while (len--)
        crc = (crc >> 8) ^ table[0][(crc ^ *p++) & 0xff];
    return ~crc;
}
