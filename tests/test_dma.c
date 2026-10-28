/* DMA pool allocator on ordinary memory with identity IOVAs (no device needed). */
#include "dma.h"
#include "test.h"

#include <stdint.h>

static struct nvm_dma_pool make_pool(size_t mib, int contiguous)
{
    struct nvm_iova_ops ops;
    nvm_iova_ops_identity(&ops);
    ops.contiguous = contiguous;
    struct nvm_dma_pool p;
    /* The non-contiguous mode needs hugetlb in production; here identity
     * IOVAs keep translation testable while exercising the chunk rule. */
    if (!contiguous) {
        ops.contiguous = 1;
        REQUIRE(nvm_dma_pool_init(&p, mib << 20, 0, &ops) == 0);
        p.ops.contiguous = 0;
    } else {
        REQUIRE(nvm_dma_pool_init(&p, mib << 20, 0, &ops) == 0);
    }
    return p;
}

TEST(alloc_is_page_aligned_zeroed_and_translated)
{
    struct nvm_dma_pool p = make_pool(4, 1);
    uint64_t iova = 0;
    uint8_t *a = nvm_dma_pool_alloc(&p, 100, &iova);
    REQUIRE(a);
    CHECK_EQ((uintptr_t)a % 4096, 0);
    CHECK_EQ(iova, (uint64_t)(uintptr_t)a);
    CHECK_EQ(nvm_dma_pool_iova(&p, a + 123), iova + 123);
    CHECK(nvm_dma_pool_virt(&p, iova + 4000) == a + 4000);
    int nonzero = 0;
    for (int i = 0; i < 4096; i++)
        nonzero += a[i] != 0;
    CHECK_EQ(nonzero, 0);
    memset(a, 0xab, 4096);
    nvm_dma_pool_free(&p, a);
    uint8_t *b = nvm_dma_pool_alloc(&p, 4096, NULL);
    CHECK(b == a);
    CHECK_EQ(b[0], 0); /* re-zeroed */
    nvm_dma_pool_free(&p, b);
    CHECK_EQ(p.used_pages, 0);
    CHECK_EQ(p.nfree, 1);
    nvm_dma_pool_fini(&p);
}

TEST(lookup_outside_pool)
{
    struct nvm_dma_pool p = make_pool(2, 1);
    int x = 0;
    CHECK_EQ(nvm_dma_pool_iova(&p, &x), UINT64_MAX);
    CHECK_EQ(nvm_dma_pool_iova(&p, p.base + p.size), UINT64_MAX);
    CHECK(nvm_dma_pool_virt(&p, 1) == NULL);
    nvm_dma_pool_fini(&p);
}

TEST(exhaustion_and_coalescing)
{
    struct nvm_dma_pool p = make_pool(2, 1); /* 512 pages */
    void *v[512];
    for (int i = 0; i < 512; i++) {
        v[i] = nvm_dma_pool_alloc(&p, 4096, NULL);
        REQUIRE(v[i]);
    }
    CHECK(nvm_dma_pool_alloc(&p, 1, NULL) == NULL);
    CHECK_EQ(p.nfree, 0);
    /* Free every other page: 256 separate extents, no 2-page hole anywhere. */
    for (int i = 0; i < 512; i += 2)
        nvm_dma_pool_free(&p, v[i]);
    CHECK_EQ(p.nfree, 256);
    CHECK(nvm_dma_pool_alloc(&p, 8192, NULL) == NULL);
    /* Freeing the rest must coalesce back into one extent. */
    for (int i = 1; i < 512; i += 2)
        nvm_dma_pool_free(&p, v[i]);
    CHECK_EQ(p.nfree, 1);
    CHECK_EQ(p.free[0].npages, 512);
    void *all = nvm_dma_pool_alloc(&p, 2u << 20, NULL);
    CHECK(all == p.base);
    nvm_dma_pool_free(&p, all);
    nvm_dma_pool_fini(&p);
}

TEST(chunk_rule_without_contiguous_iova)
{
    struct nvm_dma_pool p = make_pool(4, 0); /* two 2 MiB chunks */
    /* Leave 2 pages at the end of chunk 0, then ask for 3: must land in chunk 1. */
    void *a = nvm_dma_pool_alloc(&p, 510 * 4096, NULL);
    REQUIRE(a == p.base);
    uint8_t *b = nvm_dma_pool_alloc(&p, 3 * 4096, NULL);
    REQUIRE(b);
    CHECK(b == p.base + (2u << 20));
    /* The 2-page hole is still usable for something that fits. */
    uint8_t *c = nvm_dma_pool_alloc(&p, 2 * 4096, NULL);
    CHECK(c == p.base + 510 * 4096);
    /* Larger than a chunk is impossible without contiguous IOVAs. */
    CHECK(nvm_dma_pool_alloc(&p, (2u << 20) + 4096, NULL) == NULL);
    nvm_dma_pool_free(&p, a);
    nvm_dma_pool_free(&p, b);
    nvm_dma_pool_free(&p, c);
    CHECK_EQ(p.nfree, 1);
    nvm_dma_pool_fini(&p);
}

TEST(random_alloc_free_never_overlaps)
{
    struct nvm_dma_pool p = make_pool(8, 1);
    enum { N = 64 };
    uint8_t *ptr[N] = {0};
    size_t len[N] = {0};
    uint64_t seed = 12345;
    for (int iter = 0; iter < 20000; iter++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        int i = (int)((seed >> 33) % N);
        if (ptr[i]) {
            /* Every byte must still hold this slot's tag: nobody overlapped it. */
            for (size_t k = 0; k < len[i]; k += 512)
                if (ptr[i][k] != (uint8_t)i)
                    CHECK(!"overlap detected");
            nvm_dma_pool_free(&p, ptr[i]);
            ptr[i] = NULL;
        } else {
            len[i] = 1 + (size_t)((seed >> 13) % (300 * 1024));
            ptr[i] = nvm_dma_pool_alloc(&p, len[i], NULL);
            if (ptr[i])
                memset(ptr[i], i, len[i]);
        }
    }
    for (int i = 0; i < N; i++)
        nvm_dma_pool_free(&p, ptr[i]);
    CHECK_EQ(p.used_pages, 0);
    CHECK_EQ(p.nfree, 1);
    nvm_dma_pool_fini(&p);
}

TEST_MAIN()
