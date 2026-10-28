/*
 * I/O path smoke test against a real controller: synchronous writes and reads
 * at every PRP shape (PRP1 only, PRP1+PRP2, PRP list), argument rejection,
 * a batched async submission rung with one doorbell, and Flush.
 * Usage: io_smoke <bdf>   (overwrites the first 16 MiB of namespace 1)
 */
#include "nvmetal/nvmetal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define EXPECT(cond, ...)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static void fill(uint8_t *p, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; i += 4) {
        uint32_t v = seed * 2654435761u + (uint32_t)i;
        memcpy(p + i, &v, 4);
    }
}

static int done_count;
static void count_cb(void *arg, const struct nvm_cpl *cpl)
{
    (void)arg;
    if (nvm_status_is_error(cpl->status))
        failures++;
    done_count++;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <bdf>\n", argv[0]);
        return 2;
    }
    struct nvm_ctrl *c;
    int rc = nvm_ctrl_open(argv[1], NULL, &c);
    if (rc) {
        fprintf(stderr, "open: %s\n", strerror(-rc));
        return 1;
    }
    const struct nvm_ctrl_info *ci = nvm_ctrl_info(c);
    const struct nvm_ns_info *ns = nvm_ctrl_ns_at(c, 0);
    struct nvm_qpair *qp;
    struct nvm_qpair_opts qo = {.depth = 64};
    rc = nvm_qpair_alloc(c, &qo, &qp);
    if (rc || !ns) {
        fprintf(stderr, "qpair: %s\n", strerror(-rc));
        return 1;
    }
    uint32_t bs = ns->lba_size, max = ci->max_xfer;
    uint8_t *wbuf = nvm_dma_alloc(c, max, NULL);
    uint8_t *rbuf = nvm_dma_alloc(c, max, NULL);

    /* Sizes chosen to hit PRP1 only, PRP1+PRP2, a list, and the MDTS limit. */
    size_t sizes[] = {bs, 4096, 8192, 12288, 65536 + 4096, 131072, max};
    uint64_t lba = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t len = sizes[i];
        uint32_t nlb = (uint32_t)(len / bs);
        fill(wbuf, len, (uint32_t)i + 1);
        memset(rbuf, 0, len);
        rc = nvm_write_sync(qp, ns->nsid, wbuf, lba, nlb);
        EXPECT(rc == 0, "write %zu bytes: %d", len, rc);
        rc = nvm_read_sync(qp, ns->nsid, rbuf, lba, nlb);
        EXPECT(rc == 0, "read %zu bytes: %d", len, rc);
        EXPECT(memcmp(wbuf, rbuf, len) == 0, "data mismatch at %zu bytes", len);
        printf("  rw %7zu bytes at lba %llu ok\n", len, (unsigned long long)lba);
        lba += nlb;
    }

    /* Argument checks: none of these may reach the device. */
    EXPECT(nvm_read(qp, ns->nsid, rbuf + bs, 0, 1, NULL, NULL) == -EINVAL, "misaligned accepted");
    EXPECT(nvm_read(qp, ns->nsid, rbuf, 0, max / bs + 1, NULL, NULL) == -EINVAL,
           "oversize accepted");
    static uint8_t not_dma[8192] __attribute__((aligned(4096)));
    EXPECT(nvm_read(qp, ns->nsid, not_dma, 0, 1, NULL, NULL) == -EINVAL, "non-DMA accepted");
    EXPECT(nvm_read(qp, 999, rbuf, 0, 1, NULL, NULL) == -ENODEV, "bad nsid accepted");
    EXPECT(nvm_qpair_outstanding(qp) == 0, "rejected commands left outstanding");

    /* 32 async 4 KiB writes queued, then one poll call rings once. */
    uint8_t *batch = nvm_dma_alloc(c, 32 * 4096, NULL);
    fill(batch, 32 * 4096, 77);
    struct nvm_qpair_stats before, after;
    nvm_qpair_get_stats(qp, &before);
    for (int i = 0; i < 32; i++)
        EXPECT(nvm_write(qp, ns->nsid, batch + i * 4096, 8192 + (uint64_t)i * (4096 / bs),
                         4096 / bs, count_cb, NULL) == 0,
               "queue write %d", i);
    while (done_count < 32)
        nvm_qpair_poll(qp, 64);
    nvm_qpair_get_stats(qp, &after);
    EXPECT(after.doorbells - before.doorbells == 1, "batch used %llu doorbells",
           (unsigned long long)(after.doorbells - before.doorbells));
    rc = nvm_read_sync(qp, ns->nsid, rbuf, 8192, 32 * 4096 / bs);
    EXPECT(rc == 0 && memcmp(rbuf, batch, 32 * 4096) == 0, "batch readback");
    printf("  batch of 32 writes: %llu doorbell write(s)\n",
           (unsigned long long)(after.doorbells - before.doorbells));

    EXPECT(nvm_flush_sync(qp, ns->nsid) == 0, "flush");

    nvm_dma_free(c, batch);
    nvm_dma_free(c, wbuf);
    nvm_dma_free(c, rbuf);
    nvm_qpair_free(qp);
    nvm_ctrl_close(c);
    printf("%s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures != 0;
}
