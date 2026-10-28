/*
 * PRP (Physical Region Page) construction, §4.3 "Physical Region Page Entry
 * and List". nvmetal requires page-aligned, IOVA-contiguous buffers, so each
 * PRP entry is simply the IOVA of the next 4 KiB page.
 */
#ifndef NVMETAL_PRP_H
#define NVMETAL_PRP_H

#include <stddef.h>
#include <stdint.h>

#define NVM_PRP_PAGE_SIZE 4096u
#define NVM_PRP_PER_PAGE  (NVM_PRP_PAGE_SIZE / sizeof(uint64_t)) /* 512 */

/* One 4 KiB PRP list page: where the driver writes it and where the device reads it. */
struct nvm_prp_page {
    uint64_t *va;
    uint64_t iova;
};

/* PRP list pages needed for a transfer of len bytes (0 when PRP1/PRP2 suffice). */
unsigned nvm_prp_list_pages(size_t len);

/*
 * Fills prp1/prp2 for [iova, iova + len), writing list entries into pages[]
 * when more than two memory pages are touched. Returns 0, -EINVAL for a
 * zero length or an iova that is not 4 KiB aligned, or -ENOSPC when npages
 * list pages are not enough.
 */
int nvm_prp_build(uint64_t iova, size_t len, const struct nvm_prp_page *pages, unsigned npages,
                  uint64_t *prp1, uint64_t *prp2);

#endif
