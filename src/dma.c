#include "dma.h"
#include "util.h"
#include "vfio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGES_PER_CHUNK (NVM_HUGE_SIZE / NVM_PAGE_SIZE)

/* ---- IOVA sources ---- */

static int vfio_map(void *ctx, void *va, size_t len, uint64_t *iova)
{
    struct nvm_iova_vfio_ctx *c = ctx;
    if (c->next + len > c->vfio->iova_limit)
        return -ENOSPC;
    int rc = nvm_vfio_dma_map(c->vfio, va, len, c->next);
    if (rc)
        return rc;
    *iova = c->next;
    c->next += len;
    return 0;
}

static void vfio_unmap(void *ctx, void *va, size_t len, uint64_t iova)
{
    struct nvm_iova_vfio_ctx *c = ctx;
    (void)va;
    nvm_vfio_dma_unmap(c->vfio, iova, len);
}

void nvm_iova_ops_vfio(struct nvm_iova_ops *ops, struct nvm_iova_vfio_ctx *ctx,
                       struct nvm_vfio *vfio)
{
    ctx->vfio = vfio;
    ctx->next = vfio->iova_base;
    *ops = (struct nvm_iova_ops){
        .name = "iommu", .map = vfio_map, .unmap = vfio_unmap, .ctx = ctx, .contiguous = 1};
}

int nvm_virt_to_phys(const void *va, uint64_t *pa)
{
    /*
     * /proc/self/pagemap: one 64-bit entry per virtual page; bit 63 = present,
     * bits 54:0 = page frame number. The PFN reads as 0 without CAP_SYS_ADMIN.
     */
    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    uint64_t entry;
    off_t off = (off_t)((uintptr_t)va / NVM_PAGE_SIZE * sizeof(entry));
    ssize_t n = pread(fd, &entry, sizeof(entry), off);
    close(fd);
    if (n != sizeof(entry))
        return -EIO;
    uint64_t pfn = entry & ((1ull << 55) - 1);
    if (!(entry >> 63) || pfn == 0)
        return -EPERM;
    *pa = pfn * NVM_PAGE_SIZE + ((uintptr_t)va & (NVM_PAGE_SIZE - 1));
    return 0;
}

static int phys_map(void *ctx, void *va, size_t len, uint64_t *iova)
{
    (void)ctx;
    (void)len;
    /* A 2 MiB hugepage is physically contiguous, so one lookup covers the chunk. */
    int rc = nvm_virt_to_phys(va, iova);
    if (rc)
        nvm_err("pagemap lookup failed (%s): no-IOMMU mode needs root", strerror(-rc));
    return rc;
}

static void phys_unmap(void *ctx, void *va, size_t len, uint64_t iova)
{
    (void)ctx;
    (void)va;
    (void)len;
    (void)iova;
}

void nvm_iova_ops_phys(struct nvm_iova_ops *ops)
{
    *ops = (struct nvm_iova_ops){
        .name = "phys", .map = phys_map, .unmap = phys_unmap, .ctx = NULL, .contiguous = 0};
}

static int ident_map(void *ctx, void *va, size_t len, uint64_t *iova)
{
    (void)ctx;
    (void)len;
    *iova = (uint64_t)(uintptr_t)va;
    return 0;
}

void nvm_iova_ops_identity(struct nvm_iova_ops *ops)
{
    *ops = (struct nvm_iova_ops){
        .name = "identity", .map = ident_map, .unmap = phys_unmap, .ctx = NULL, .contiguous = 1};
}

/* ---- Pool ---- */

int nvm_dma_pool_init(struct nvm_dma_pool *p, size_t size, int use_hugetlb,
                      const struct nvm_iova_ops *ops)
{
    memset(p, 0, sizeof(*p));
    size = NVM_ALIGN_UP(size, (size_t)NVM_HUGE_SIZE);
    if (size == 0 || size / NVM_PAGE_SIZE > UINT32_MAX)
        return -EINVAL;
    if (!ops->contiguous && !use_hugetlb) {
        nvm_err("physical-address DMA needs hugepages: ordinary pages are not contiguous");
        return -EINVAL;
    }
    p->ops = *ops;
    p->size = size;
    p->npages = (uint32_t)(size / NVM_PAGE_SIZE);
    p->nchunks = (uint32_t)(size / NVM_HUGE_SIZE);
    p->hugetlb = use_hugetlb;

    if (use_hugetlb) {
        /* MAP_POPULATE faults every hugepage in now, so the physical pages (and
         * their pagemap entries) exist before they are mapped for DMA. */
        void *m = mmap(NULL, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0);
        if (m == MAP_FAILED) {
            int e = errno;
            nvm_err("mmap %zu MiB of 2 MiB hugepages: %s (reserve more with "
                    "hugepages=N on the kernel command line or vm.nr_hugepages)",
                    size >> 20, strerror(e));
            return -e;
        }
        p->base = m;
    } else {
        void *m = NULL;
        int e = posix_memalign(&m, NVM_HUGE_SIZE, size);
        if (e)
            return -e;
        memset(m, 0, size);
        p->base = m;
    }
    /* Device-visible memory must never be paged out under the device. Hugetlb
     * pages cannot be swapped anyway; mlock covers the test fallback. */
    if (mlock(p->base, size) != 0)
        nvm_debug("mlock DMA pool: %s", strerror(errno));

    int rc = -ENOMEM;
    p->chunk_iova = calloc(p->nchunks, sizeof(*p->chunk_iova));
    p->alloc_len = calloc(p->npages, sizeof(*p->alloc_len));
    p->free_cap = 16;
    p->free = calloc(p->free_cap, sizeof(*p->free));
    if (!p->chunk_iova || !p->alloc_len || !p->free)
        goto fail;

    uint32_t mapped = 0;
    for (; mapped < p->nchunks; mapped++) {
        void *va = p->base + (size_t)mapped * NVM_HUGE_SIZE;
        rc = ops->map(ops->ctx, va, NVM_HUGE_SIZE, &p->chunk_iova[mapped]);
        if (rc)
            break;
    }
    if (rc) {
        while (mapped--)
            ops->unmap(ops->ctx, p->base + (size_t)mapped * NVM_HUGE_SIZE, NVM_HUGE_SIZE,
                       p->chunk_iova[mapped]);
        goto fail;
    }

    p->free[0] = (struct nvm_dma_extent){0, p->npages};
    p->nfree = 1;
    pthread_mutex_init(&p->lock, NULL);
    nvm_info("DMA pool: %zu MiB %s, iova %s, first chunk at 0x%llx", size >> 20,
             use_hugetlb ? "hugetlb" : "heap", ops->name, (unsigned long long)p->chunk_iova[0]);
    return 0;

fail:
    free(p->chunk_iova);
    free(p->alloc_len);
    free(p->free);
    if (use_hugetlb)
        munmap(p->base, size);
    else
        free(p->base);
    memset(p, 0, sizeof(*p));
    return rc;
}

void nvm_dma_pool_fini(struct nvm_dma_pool *p)
{
    if (!p->base)
        return;
    for (uint32_t i = 0; i < p->nchunks; i++)
        p->ops.unmap(p->ops.ctx, p->base + (size_t)i * NVM_HUGE_SIZE, NVM_HUGE_SIZE,
                     p->chunk_iova[i]);
    if (p->hugetlb) {
        munmap(p->base, p->size);
    } else {
        munlock(p->base, p->size);
        free(p->base);
    }
    free(p->chunk_iova);
    free(p->alloc_len);
    free(p->free);
    pthread_mutex_destroy(&p->lock);
    memset(p, 0, sizeof(*p));
}

/*
 * First fit within extent e for n pages. Without contiguous IOVAs an
 * allocation must stay inside one 2 MiB chunk, because only the chunk is
 * known to be physically contiguous.
 */
static int fit_in_extent(const struct nvm_dma_pool *p, const struct nvm_dma_extent *e, uint32_t n,
                         uint32_t *start)
{
    uint32_t s = e->start, end = e->start + e->npages;
    if (!p->ops.contiguous) {
        if (n > PAGES_PER_CHUNK)
            return 0;
        if (s / PAGES_PER_CHUNK != (s + n - 1) / PAGES_PER_CHUNK)
            s = NVM_ALIGN_UP(s, PAGES_PER_CHUNK);
    }
    if (s + n > end)
        return 0;
    *start = s;
    return 1;
}

static int free_reserve(struct nvm_dma_pool *p, uint32_t extra)
{
    if (p->nfree + extra <= p->free_cap)
        return 0;
    uint32_t cap = p->free_cap * 2;
    while (cap < p->nfree + extra)
        cap *= 2;
    struct nvm_dma_extent *n = realloc(p->free, cap * sizeof(*n));
    if (!n)
        return -ENOMEM;
    p->free = n;
    p->free_cap = cap;
    return 0;
}

void *nvm_dma_pool_alloc(struct nvm_dma_pool *p, size_t size, uint64_t *iova)
{
    if (size == 0 || size > p->size)
        return NULL;
    uint32_t n = (uint32_t)(NVM_ALIGN_UP(size, (size_t)NVM_PAGE_SIZE) / NVM_PAGE_SIZE);
    void *va = NULL;

    pthread_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < p->nfree; i++) {
        uint32_t s;
        if (!fit_in_extent(p, &p->free[i], n, &s))
            continue;
        struct nvm_dma_extent e = p->free[i];
        uint32_t head = s - e.start, tail = e.start + e.npages - (s + n);
        if (head && tail) {
            /* Splitting in the middle leaves two extents. */
            if (free_reserve(p, 1))
                break;
            memmove(&p->free[i + 2], &p->free[i + 1], (p->nfree - i - 1) * sizeof(*p->free));
            p->free[i] = (struct nvm_dma_extent){e.start, head};
            p->free[i + 1] = (struct nvm_dma_extent){s + n, tail};
            p->nfree++;
        } else if (head) {
            p->free[i].npages = head;
        } else if (tail) {
            p->free[i] = (struct nvm_dma_extent){s + n, tail};
        } else {
            memmove(&p->free[i], &p->free[i + 1], (p->nfree - i - 1) * sizeof(*p->free));
            p->nfree--;
        }
        p->alloc_len[s] = n;
        p->used_pages += n;
        va = p->base + (size_t)s * NVM_PAGE_SIZE;
        break;
    }
    pthread_mutex_unlock(&p->lock);

    if (va) {
        memset(va, 0, (size_t)n * NVM_PAGE_SIZE);
        if (iova)
            *iova = nvm_dma_pool_iova(p, va);
    }
    return va;
}

void nvm_dma_pool_free(struct nvm_dma_pool *p, void *va)
{
    if (!va)
        return;
    uintptr_t off = (uintptr_t)va - (uintptr_t)p->base;
    if (off >= p->size || off % NVM_PAGE_SIZE) {
        nvm_err("dma free of %p: not an allocation from this pool", va);
        abort();
    }
    uint32_t s = (uint32_t)(off / NVM_PAGE_SIZE);

    pthread_mutex_lock(&p->lock);
    uint32_t n = p->alloc_len[s];
    if (n == 0) {
        pthread_mutex_unlock(&p->lock);
        nvm_err("dma double free or bad pointer %p", va);
        abort();
    }
    p->alloc_len[s] = 0;
    p->used_pages -= n;

    /* Binary search for the insertion point, then coalesce with neighbours. */
    uint32_t lo = 0, hi = p->nfree;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (p->free[mid].start < s)
            lo = mid + 1;
        else
            hi = mid;
    }
    int merge_prev = lo > 0 && p->free[lo - 1].start + p->free[lo - 1].npages == s;
    int merge_next = lo < p->nfree && s + n == p->free[lo].start;
    if (merge_prev && merge_next) {
        p->free[lo - 1].npages += n + p->free[lo].npages;
        memmove(&p->free[lo], &p->free[lo + 1], (p->nfree - lo - 1) * sizeof(*p->free));
        p->nfree--;
    } else if (merge_prev) {
        p->free[lo - 1].npages += n;
    } else if (merge_next) {
        p->free[lo].start = s;
        p->free[lo].npages += n;
    } else {
        if (free_reserve(p, 1)) {
            /* Leaking pages is better than corrupting the free list. */
            pthread_mutex_unlock(&p->lock);
            nvm_err("dma free: out of memory growing the free list");
            return;
        }
        memmove(&p->free[lo + 1], &p->free[lo], (p->nfree - lo) * sizeof(*p->free));
        p->free[lo] = (struct nvm_dma_extent){s, n};
        p->nfree++;
    }
    pthread_mutex_unlock(&p->lock);
}

void *nvm_dma_pool_virt(const struct nvm_dma_pool *p, uint64_t iova)
{
    for (uint32_t i = 0; i < p->nchunks; i++) {
        uint64_t c = p->chunk_iova[i];
        if (iova >= c && iova < c + NVM_HUGE_SIZE)
            return p->base + (size_t)i * NVM_HUGE_SIZE + (iova - c);
    }
    return NULL;
}
