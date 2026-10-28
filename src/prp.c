#include "prp.h"

#include <errno.h>

unsigned nvm_prp_list_pages(size_t len)
{
    size_t mem_pages = (len + NVM_PRP_PAGE_SIZE - 1) / NVM_PRP_PAGE_SIZE;
    if (mem_pages <= 2)
        return 0;
    size_t entries = mem_pages - 1; /* PRP1 covers the first page */
    if (entries <= NVM_PRP_PER_PAGE)
        return 1;
    /* Every list page but the last spends its final slot on the chain pointer. */
    return (unsigned)(1 + (entries - NVM_PRP_PER_PAGE + (NVM_PRP_PER_PAGE - 1) - 1) /
                              (NVM_PRP_PER_PAGE - 1));
}

int nvm_prp_build(uint64_t iova, size_t len, const struct nvm_prp_page *pages, unsigned npages,
                  uint64_t *prp1, uint64_t *prp2)
{
    /*
     * §4.3: PRP1 may carry a dword-aligned offset, but every later entry must
     * be page aligned. Requiring a page-aligned start keeps every entry a
     * whole page and rules out buffers the device would reject or, worse,
     * transfer to the wrong place.
     */
    if (len == 0 || (iova & (NVM_PRP_PAGE_SIZE - 1)))
        return -EINVAL;
    size_t mem_pages = (len + NVM_PRP_PAGE_SIZE - 1) / NVM_PRP_PAGE_SIZE;
    *prp1 = iova;
    if (mem_pages == 1) {
        *prp2 = 0;
        return 0;
    }
    if (mem_pages == 2) {
        /* Exactly two pages: PRP2 is the second page itself, not a list. */
        *prp2 = iova + NVM_PRP_PAGE_SIZE;
        return 0;
    }
    if (nvm_prp_list_pages(len) > npages)
        return -ENOSPC;

    /* More than two pages: PRP2 points at a list of the remaining pages. */
    size_t remaining = mem_pages - 1;
    uint64_t next = iova + NVM_PRP_PAGE_SIZE;
    unsigned lp = 0;
    *prp2 = pages[0].iova;
    for (;;) {
        uint64_t *list = pages[lp].va;
        if (remaining <= NVM_PRP_PER_PAGE) {
            for (size_t i = 0; i < remaining; i++, next += NVM_PRP_PAGE_SIZE)
                list[i] = next;
            return 0;
        }
        /* Fill 511 entries, chain to the next list page through the last one. */
        for (size_t i = 0; i < NVM_PRP_PER_PAGE - 1; i++, next += NVM_PRP_PAGE_SIZE)
            list[i] = next;
        remaining -= NVM_PRP_PER_PAGE - 1;
        list[NVM_PRP_PER_PAGE - 1] = pages[lp + 1].iova;
        lp++;
    }
}
