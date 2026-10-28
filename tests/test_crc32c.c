/* CRC-32C known answers (RFC 3720 appendix B.4 and the common "123456789" check). */
#include "crc32c.h"
#include "test.h"

static uint32_t bitwise(const uint8_t *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0x82f63b78u & (0u - (c & 1)));
    }
    return ~c;
}

TEST(check_value)
{
    CHECK_EQ(crc32c(0, "123456789", 9), 0xe3069283);
    CHECK_EQ(crc32c(0, "", 0), 0);
}

TEST(rfc3720_vectors)
{
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(crc32c(0, buf, 32), 0x8a9136aa);
    memset(buf, 0xff, sizeof(buf));
    CHECK_EQ(crc32c(0, buf, 32), 0x62a8ab43);
    for (int i = 0; i < 32; i++)
        buf[i] = (uint8_t)i;
    CHECK_EQ(crc32c(0, buf, 32), 0x46dd794e);
    for (int i = 0; i < 32; i++)
        buf[i] = (uint8_t)(31 - i);
    CHECK_EQ(crc32c(0, buf, 32), 0x113fdb5c);
}

TEST(matches_bitwise_at_every_alignment_and_length)
{
    static uint8_t data[300];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 131 + 7);
    int bad = 0;
    for (size_t off = 0; off < 9; off++)
        for (size_t len = 0; len + off <= sizeof(data); len += 7)
            bad += crc32c(0, data + off, len) != bitwise(data + off, len);
    CHECK_EQ(bad, 0);
    /* Incremental use equals one-shot. */
    CHECK_EQ(crc32c(crc32c(0, data, 100), data + 100, 200), crc32c(0, data, 300));
}

TEST_MAIN()
