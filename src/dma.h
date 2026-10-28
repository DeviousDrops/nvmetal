/*
 * DMA memory: a pool of 2 MiB hugepages, mlocked, carved into 4 KiB pages by a
 * free-list allocator, with O(1) virtual-to-IOVA translation.
 */
#ifndef NVMETAL_DMA_H
#define NVMETAL_DMA_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

struct nvm_vfio;

/*
 * Where IOVAs come from. The pool calls map() once per 2 MiB chunk at start-up
 * and unmap() at teardown; nothing on the I/O path goes through this.
 */
struct nvm_iova_ops {
    const char *name;
    int (*map)(void *ctx, void *va, size_t len, uint64_t *iova);
    void (*unmap)(void *ctx, void *va, size_t len, uint64_t iova);
    void *ctx;
    /* Consecutive chunks are guaranteed consecutive IOVAs, so allocations may
     * span chunks. False for physical addresses (no-IOMMU mode). */
    int contiguous;
};

/* IOMMU mode: VFIO_IOMMU_MAP_DMA each chunk at increasing IOVAs from the
 * container's window. ctx must outlive the pool. */
struct nvm_iova_vfio_ctx {
    struct nvm_vfio *vfio;
    uint64_t next;
};
void nvm_iova_ops_vfio(struct nvm_iova_ops *ops, struct nvm_iova_vfio_ctx *ctx,
                       struct nvm_vfio *vfio);

/* No-IOMMU mode: IOVA = physical address from /proc/self/pagemap (needs root). */
void nvm_iova_ops_phys(struct nvm_iova_ops *ops);

/* Tests: IOVA = virtual address, so a fake device can dereference IOVAs. */
void nvm_iova_ops_identity(struct nvm_iova_ops *ops);

/* Looks up the physical address backing va; exposed for tests and the phys ops. */
int nvm_virt_to_phys(const void *va, uint64_t *pa);

struct nvm_dma_extent {
    uint32_t start; /* page index */
    uint32_t npages;
};

struct nvm_dma_pool {
    uint8_t *base;
    size_t size;
    uint32_t npages;
    uint32_t nchunks;
    int hugetlb;
    uint64_t *chunk_iova; /* IOVA of each 2 MiB chunk */
    uint32_t *alloc_len;  /* at an allocation's first page: its length in pages */
    struct nvm_dma_extent *free; /* sorted by start, never adjacent (coalesced) */
    uint32_t nfree;
    uint32_t free_cap;
    uint32_t used_pages;
    struct nvm_iova_ops ops;
    pthread_mutex_t lock; /* allocation is not on the I/O path */
};

/*
 * size is rounded up to 2 MiB. use_hugetlb=0 backs the pool with ordinary
 * 2 MiB-aligned memory, for host tests only.
 */
int nvm_dma_pool_init(struct nvm_dma_pool *p, size_t size, int use_hugetlb,
                      const struct nvm_iova_ops *ops);
void nvm_dma_pool_fini(struct nvm_dma_pool *p);

/* Page-aligned, zeroed, IOVA-contiguous memory; NULL when the pool is exhausted. */
void *nvm_dma_pool_alloc(struct nvm_dma_pool *p, size_t size, uint64_t *iova);
void nvm_dma_pool_free(struct nvm_dma_pool *p, void *va);

/* UINT64_MAX when va is not inside the pool. */
static inline uint64_t nvm_dma_pool_iova(const struct nvm_dma_pool *p, const void *va)
{
    uintptr_t off = (uintptr_t)va - (uintptr_t)p->base;
    if (off >= p->size)
        return UINT64_MAX;
    return p->chunk_iova[off >> NVM_HUGE_SHIFT] + (off & (NVM_HUGE_SIZE - 1));
}

/* NULL when iova is not inside the pool. */
void *nvm_dma_pool_virt(const struct nvm_dma_pool *p, uint64_t iova);

#endif
