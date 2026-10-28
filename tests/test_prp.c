/*
 * PRP builder: every size from 1 byte to past two chained list pages is
 * built, then walked the way a controller walks it (§4.3), and the walk must
 * produce exactly the buffer's pages in order.
 */
#include "prp.h"
#include "test.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#define BUF_IOVA       0x100000000ull /* fake IOVA of the data buffer */
#define MAX_LIST_PAGES 4

static uint64_t list_mem[MAX_LIST_PAGES][NVM_PRP_PER_PAGE] __attribute__((aligned(4096)));

static void make_pages(struct nvm_prp_page *p, unsigned n)
{
    /* List pages use their host address as IOVA so the walker can follow them. */
    for (unsigned i = 0; i < n; i++)
        p[i] = (struct nvm_prp_page){list_mem[i], (uint64_t)(uintptr_t)list_mem[i]};
}

/* Device-side walk: returns the number of data pages found, storing them in out[]. */
static size_t walk(uint64_t prp1, uint64_t prp2, size_t len, uint64_t *out, size_t max)
{
    size_t pages = (len + 4095) / 4096, n = 0;
    out[n++] = prp1;
    if (pages == 1)
        return n;
    if (pages == 2) {
        out[n++] = prp2;
        return n;
    }
    const uint64_t *list = (const uint64_t *)(uintptr_t)prp2;
    size_t idx = 0;
    while (n < pages && n < max) {
        size_t left = pages - n;
        if (idx == NVM_PRP_PER_PAGE - 1 && left > 1) {
            /* Last slot of a full list page with more to come: chain pointer. */
            list = (const uint64_t *)(uintptr_t)list[idx];
            idx = 0;
            continue;
        }
        out[n++] = list[idx++];
    }
    return n;
}

static void check_len(size_t len)
{
    struct nvm_prp_page pages[MAX_LIST_PAGES];
    make_pages(pages, MAX_LIST_PAGES);
    memset(list_mem, 0xee, sizeof(list_mem));
    uint64_t prp1 = 0, prp2 = 0;
    int rc = nvm_prp_build(BUF_IOVA, len, pages, MAX_LIST_PAGES, &prp1, &prp2);
    CHECK_EQ(rc, 0);
    if (rc)
        return;
    size_t want = (len + 4095) / 4096;
    static uint64_t got[MAX_LIST_PAGES * NVM_PRP_PER_PAGE + 2];
    size_t n = walk(prp1, prp2, len, got, sizeof(got) / sizeof(got[0]));
    CHECK_EQ(n, want);
    int bad = 0;
    for (size_t i = 0; i < n; i++)
        bad += got[i] != BUF_IOVA + i * 4096;
    CHECK_EQ(bad, 0);
    if (want <= 2)
        CHECK(want == 2 ? prp2 == BUF_IOVA + 4096 : prp2 == 0);
    else
        CHECK_EQ(prp2, pages[0].iova);
}

TEST(single_page_sizes)
{
    check_len(1);
    check_len(512);
    check_len(4096);
}

TEST(two_pages_use_prp2_directly)
{
    check_len(4097);
    check_len(8192);
    CHECK_EQ(nvm_prp_list_pages(8192), 0);
}

TEST(three_pages_need_a_list)
{
    check_len(8193);
    check_len(12288);
    CHECK_EQ(nvm_prp_list_pages(12288), 1);
}

TEST(list_exactly_fills_one_page)
{
    /* 513 memory pages: PRP1 + 512 list entries, no chain pointer needed. */
    size_t len = 513 * 4096;
    CHECK_EQ(nvm_prp_list_pages(len), 1);
    check_len(len);
    CHECK_EQ(list_mem[0][511], BUF_IOVA + 512 * 4096); /* last slot is data */
}

TEST(list_crosses_into_second_page)
{
    /* 514 memory pages: 513 entries, so the first list page chains. */
    size_t len = 514 * 4096;
    CHECK_EQ(nvm_prp_list_pages(len), 2);
    check_len(len);
    CHECK_EQ(list_mem[0][511], (uint64_t)(uintptr_t)list_mem[1]);
    CHECK_EQ(list_mem[1][0], BUF_IOVA + 512 * 4096);
    CHECK_EQ(list_mem[1][1], BUF_IOVA + 513 * 4096);
}

TEST(list_spans_three_pages)
{
    /* 1 + 511 + 511 + 2 pages. */
    size_t len = (1 + 511 + 511 + 2) * 4096ul;
    CHECK_EQ(nvm_prp_list_pages(len), 3);
    check_len(len);
    CHECK_EQ(list_mem[1][511], (uint64_t)(uintptr_t)list_mem[2]);
}

TEST(every_page_count_up_to_four_list_pages)
{
    size_t max_pages = 1 + 511 * 3 + 512;
    for (size_t pg = 1; pg <= max_pages; pg++) {
        check_len(pg * 4096);
        check_len(pg * 4096 - 511); /* partial last page */
    }
    CHECK_EQ(nvm_prp_list_pages(max_pages * 4096), 4);
    CHECK_EQ(nvm_prp_list_pages((max_pages + 1) * 4096), 5);
}

TEST(rejects_bad_input)
{
    struct nvm_prp_page pages[1];
    make_pages(pages, 1);
    uint64_t a, b;
    CHECK_EQ(nvm_prp_build(BUF_IOVA + 512, 4096, pages, 1, &a, &b), -EINVAL);
    CHECK_EQ(nvm_prp_build(BUF_IOVA + 4, 4096, pages, 1, &a, &b), -EINVAL);
    CHECK_EQ(nvm_prp_build(BUF_IOVA, 0, pages, 1, &a, &b), -EINVAL);
    /* 514 pages need two list pages; one is not enough. */
    CHECK_EQ(nvm_prp_build(BUF_IOVA, 514 * 4096, pages, 1, &a, &b), -ENOSPC);
    CHECK_EQ(nvm_prp_build(BUF_IOVA, 3 * 4096, pages, 0, &a, &b), -ENOSPC);
    /* Two pages need no list at all. */
    CHECK_EQ(nvm_prp_build(BUF_IOVA, 2 * 4096, NULL, 0, &a, &b), 0);
}

TEST_MAIN()
