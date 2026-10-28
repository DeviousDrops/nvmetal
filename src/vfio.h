/* Thin wrapper over the VFIO container/group/device API (Documentation/driver-api/vfio.rst). */
#ifndef NVMETAL_VFIO_H
#define NVMETAL_VFIO_H

#include <stddef.h>
#include <stdint.h>

struct nvm_vfio {
    int container;
    int group;
    int device;
    int noiommu; /* group opened through /dev/vfio/noiommu-N: no DMA translation */
    void *bar0;
    size_t bar0_size;
    uint64_t cfg_offset; /* file offset of the PCI config region in the device fd */
    uint32_t msix_count; /* MSI-X vectors the device exposes */
    uint64_t iova_base;  /* start of the IOVA window nvmetal allocates from */
    uint64_t iova_limit; /* end (exclusive) */
    char bdf[32];
};

/* Opens container, group and device for the PCI function at bdf ("0000:00:04.0"). */
int nvm_vfio_open(struct nvm_vfio *v, const char *bdf);
void nvm_vfio_close(struct nvm_vfio *v);

int nvm_vfio_cfg_read(struct nvm_vfio *v, uint32_t off, void *buf, size_t len);
int nvm_vfio_cfg_write(struct nvm_vfio *v, uint32_t off, const void *buf, size_t len);

/* Sets Memory Space Enable and Bus Master Enable in the PCI Command register. */
int nvm_vfio_enable_bus_master(struct nvm_vfio *v);

/* IOMMU mode only: map [va, va+len) at iova for device reads and writes. */
int nvm_vfio_dma_map(struct nvm_vfio *v, void *va, size_t len, uint64_t iova);
int nvm_vfio_dma_unmap(struct nvm_vfio *v, uint64_t iova, size_t len);

#endif
