/*
 * Guest check for the VFIO and DMA layers: open the device, map BAR0, read
 * CAP/VS, enable bus mastering, build a hugepage DMA pool mapped through the
 * IOMMU (or physical addresses in no-IOMMU mode) and check the translations.
 * Usage: vfio_probe <bdf>
 */
#include "dma.h"
#include "mmio.h"
#include "nvme_spec.h"
#include "vfio.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <bdf>\n", argv[0]);
        return 2;
    }
    struct nvm_vfio v;
    int rc = nvm_vfio_open(&v, argv[1]);
    if (rc) {
        fprintf(stderr, "vfio open failed: %s\n", strerror(-rc));
        return 1;
    }
    uint64_t cap = nvm_mmio_read64(v.bar0, NVME_REG_CAP);
    uint32_t vs = nvm_mmio_read32(v.bar0, NVME_REG_VS);
    printf("bdf %s mode %s bar0 %zu bytes msix %u\n", v.bdf, v.noiommu ? "noiommu" : "iommu",
           v.bar0_size, v.msix_count);
    printf("CAP 0x%016" PRIx64 ": MQES %u CQR %u TO %u (x500ms) DSTRD %u CSS 0x%x MPSMIN %u "
           "MPSMAX %u\n",
           cap, NVME_CAP_MQES(cap) + 1, NVME_CAP_CQR(cap), NVME_CAP_TO(cap), NVME_CAP_DSTRD(cap),
           NVME_CAP_CSS(cap), NVME_CAP_MPSMIN(cap), NVME_CAP_MPSMAX(cap));
    printf("VS %u.%u.%u\n", vs >> 16, (vs >> 8) & 0xff, vs & 0xff);
    printf("IOVA window [0x%" PRIx64 ", 0x%" PRIx64 ")\n", v.iova_base, v.iova_limit);
    int fail = 0;
    if (cap == UINT64_MAX || !(NVME_CAP_CSS(cap) & NVME_CAP_CSS_NVM)) {
        fprintf(stderr, "CAP looks wrong\n");
        fail = 1;
    }

    rc = nvm_vfio_enable_bus_master(&v);
    uint16_t cmd = 0;
    nvm_vfio_cfg_read(&v, 4, &cmd, 2);
    printf("PCI command 0x%04x (bus master %s)\n", cmd, (cmd & 4) ? "on" : "OFF");
    fail |= rc != 0 || !(cmd & 4);

    struct nvm_iova_ops ops;
    struct nvm_iova_vfio_ctx ctx;
    if (v.noiommu)
        nvm_iova_ops_phys(&ops);
    else
        nvm_iova_ops_vfio(&ops, &ctx, &v);
    struct nvm_dma_pool pool;
    rc = nvm_dma_pool_init(&pool, 16u << 20, 1, &ops);
    if (rc) {
        fprintf(stderr, "dma pool: %s\n", strerror(-rc));
        return 1;
    }
    uint64_t iova;
    uint8_t *a = nvm_dma_pool_alloc(&pool, 64 * 1024, &iova);
    uint8_t *b = nvm_dma_pool_alloc(&pool, 3u << 20, NULL);
    printf("pool: %u chunks, first iova 0x%" PRIx64 ", alloc a=%p iova 0x%" PRIx64 " b=%p\n",
           pool.nchunks, pool.chunk_iova[0], (void *)a, iova, (void *)b);
    fail |= !a || nvm_dma_pool_virt(&pool, iova + 100) != a + 100;
    fail |= nvm_dma_pool_iova(&pool, a + 4097) != iova + 4097;
    if (!v.noiommu)
        fail |= !b; /* spanning chunks is only possible with IOMMU-contiguous IOVAs */
    nvm_dma_pool_free(&pool, a);
    nvm_dma_pool_free(&pool, b);
    nvm_dma_pool_fini(&pool);
    nvm_vfio_close(&v);
    printf("%s\n", fail ? "FAIL" : "OK");
    return fail;
}
