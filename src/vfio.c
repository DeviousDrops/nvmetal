/*
 * VFIO plumbing: container -> group -> device, BAR0 mapping, PCI config space,
 * and IOMMU DMA mappings. The sequence follows the kernel's VFIO
 * documentation (Documentation/driver-api/vfio.rst, "VFIO Usage Example").
 */
#include "vfio.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/vfio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* PCI configuration header offsets (PCI Local Bus Spec 3.0 §6.2; NVMe §2.1.2 CMD). */
#define PCI_COMMAND        0x04
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4

/*
 * Default IOVA window when the IOMMU does not report its valid ranges: start
 * at 4 GiB, above the x86 MSI doorbell window at 0xfee00000 that the IOMMU
 * reserves, and stay below 2^39, the smallest address width VT-d implements.
 */
#define NVM_IOVA_DEFAULT_BASE  (1ull << 32)
#define NVM_IOVA_DEFAULT_LIMIT (1ull << 39)

static int group_number(const char *bdf, char *name, size_t len)
{
    char path[PATH_MAX], link[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n < 0) {
        nvm_err("%s: no IOMMU group (%s); bind it with scripts/bind.sh %s vfio", bdf,
                strerror(errno), bdf);
        return -errno;
    }
    link[n] = '\0';
    const char *slash = strrchr(link, '/');
    const char *g = slash ? slash + 1 : link;
    size_t glen = strlen(g);
    if (glen >= len)
        return -ENAMETOOLONG;
    memcpy(name, g, glen + 1);
    return 0;
}

/*
 * Picks an IOVA window from the type1 IOMMU's reported valid ranges
 * (VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE): the largest range at or above 4 GiB.
 */
static void choose_iova_window(struct nvm_vfio *v)
{
    v->iova_base = NVM_IOVA_DEFAULT_BASE;
    v->iova_limit = NVM_IOVA_DEFAULT_LIMIT;

    struct vfio_iommu_type1_info probe = {.argsz = sizeof(probe)};
    if (ioctl(v->container, VFIO_IOMMU_GET_INFO, &probe) < 0 || probe.argsz <= sizeof(probe))
        return;
    struct vfio_iommu_type1_info *info = calloc(1, probe.argsz);
    if (!info)
        return;
    info->argsz = probe.argsz;
    if (ioctl(v->container, VFIO_IOMMU_GET_INFO, info) < 0 ||
        !(info->flags & VFIO_IOMMU_INFO_CAPS) || !info->cap_offset)
        goto out;

    uint32_t off = info->cap_offset;
    while (off && off < info->argsz) {
        struct vfio_info_cap_header *hdr = (void *)((char *)info + off);
        if (hdr->id == VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE) {
            struct vfio_iommu_type1_info_cap_iova_range *cap = (void *)hdr;
            uint64_t best_lo = 0, best_len = 0;
            for (uint32_t i = 0; i < cap->nr_iovas; i++) {
                uint64_t lo = cap->iova_ranges[i].start;
                uint64_t hi = cap->iova_ranges[i].end; /* inclusive */
                if (hi < NVM_IOVA_DEFAULT_BASE)
                    continue;
                if (lo < NVM_IOVA_DEFAULT_BASE)
                    lo = NVM_IOVA_DEFAULT_BASE;
                if (hi - lo + 1 > best_len) {
                    best_lo = lo;
                    best_len = hi - lo + 1;
                }
            }
            if (best_len) {
                v->iova_base = NVM_ALIGN_UP(best_lo, (uint64_t)NVM_HUGE_SIZE);
                v->iova_limit = best_lo + best_len;
            }
            break;
        }
        off = hdr->next;
    }
out:
    free(info);
}

int nvm_vfio_open(struct nvm_vfio *v, const char *bdf)
{
    int rc;
    memset(v, 0, sizeof(*v));
    v->container = v->group = v->device = -1;
    snprintf(v->bdf, sizeof(v->bdf), "%s", bdf);

    char grp[64], path[PATH_MAX];
    if ((rc = group_number(bdf, grp, sizeof(grp))) < 0)
        return rc;

    /*
     * In unsafe no-IOMMU mode vfio-pci creates /dev/vfio/noiommu-N instead of
     * /dev/vfio/N, and the container must use the NOIOMMU "IOMMU" type, which
     * accepts no DMA mappings: the device sees physical addresses.
     */
    snprintf(path, sizeof(path), "/dev/vfio/noiommu-%s", grp);
    if (access(path, F_OK) == 0)
        v->noiommu = 1;
    else
        snprintf(path, sizeof(path), "/dev/vfio/%s", grp);

    v->container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
    if (v->container < 0) {
        rc = -errno;
        nvm_err("open /dev/vfio/vfio: %s", strerror(errno));
        goto fail;
    }
    if (ioctl(v->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        rc = -EINVAL;
        nvm_err("unexpected VFIO API version");
        goto fail;
    }
    int type = v->noiommu ? VFIO_NOIOMMU_IOMMU : VFIO_TYPE1v2_IOMMU;
    if (ioctl(v->container, VFIO_CHECK_EXTENSION, type) <= 0) {
        if (!v->noiommu && ioctl(v->container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) > 0) {
            type = VFIO_TYPE1_IOMMU;
        } else {
            rc = -ENOTSUP;
            nvm_err("VFIO IOMMU type %d not supported", type);
            goto fail;
        }
    }

    v->group = open(path, O_RDWR | O_CLOEXEC);
    if (v->group < 0) {
        rc = -errno;
        nvm_err("open %s: %s (is %s bound to vfio-pci, and are you root?)", path,
                strerror(errno), bdf);
        goto fail;
    }
    struct vfio_group_status gs = {.argsz = sizeof(gs)};
    if (ioctl(v->group, VFIO_GROUP_GET_STATUS, &gs) < 0 || !(gs.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        rc = -EBUSY;
        nvm_err("IOMMU group %s is not viable: bind every device in it to vfio-pci", grp);
        goto fail;
    }
    if (ioctl(v->group, VFIO_GROUP_SET_CONTAINER, &v->container) < 0 ||
        ioctl(v->container, VFIO_SET_IOMMU, type) < 0) {
        rc = -errno;
        nvm_err("attaching group %s to container: %s", grp, strerror(errno));
        goto fail;
    }
    v->device = ioctl(v->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (v->device < 0) {
        rc = -errno;
        nvm_err("VFIO_GROUP_GET_DEVICE_FD %s: %s", bdf, strerror(errno));
        goto fail;
    }

    struct vfio_region_info ri = {.argsz = sizeof(ri), .index = VFIO_PCI_BAR0_REGION_INDEX};
    if (ioctl(v->device, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) {
        rc = -errno;
        goto fail;
    }
    if (!(ri.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
        rc = -ENOTSUP;
        nvm_err("BAR0 of %s is not mmap-able", bdf);
        goto fail;
    }
    v->bar0 = mmap(NULL, ri.size, PROT_READ | PROT_WRITE, MAP_SHARED, v->device, (off_t)ri.offset);
    if (v->bar0 == MAP_FAILED) {
        v->bar0 = NULL;
        rc = -errno;
        nvm_err("mmap BAR0 (%llu bytes): %s", (unsigned long long)ri.size, strerror(errno));
        goto fail;
    }
    v->bar0_size = ri.size;

    ri = (struct vfio_region_info){.argsz = sizeof(ri), .index = VFIO_PCI_CONFIG_REGION_INDEX};
    if (ioctl(v->device, VFIO_DEVICE_GET_REGION_INFO, &ri) < 0) {
        rc = -errno;
        goto fail;
    }
    v->cfg_offset = ri.offset;

    struct vfio_irq_info ii = {.argsz = sizeof(ii), .index = VFIO_PCI_MSIX_IRQ_INDEX};
    if (ioctl(v->device, VFIO_DEVICE_GET_IRQ_INFO, &ii) == 0)
        v->msix_count = ii.count;

    if (!v->noiommu)
        choose_iova_window(v);
    nvm_info("%s: group %s%s, BAR0 %zu bytes, %u MSI-X vectors", bdf, grp,
             v->noiommu ? " (no-IOMMU)" : "", v->bar0_size, v->msix_count);
    return 0;

fail:
    nvm_vfio_close(v);
    return rc;
}

void nvm_vfio_close(struct nvm_vfio *v)
{
    if (v->bar0)
        munmap(v->bar0, v->bar0_size);
    v->bar0 = NULL;
    /* Closing the device then the group detaches it; closing the container
     * tears down every IOMMU mapping made through it. */
    if (v->device >= 0)
        close(v->device);
    if (v->group >= 0)
        close(v->group);
    if (v->container >= 0)
        close(v->container);
    v->device = v->group = v->container = -1;
}

int nvm_vfio_cfg_read(struct nvm_vfio *v, uint32_t off, void *buf, size_t len)
{
    ssize_t n = pread(v->device, buf, len, (off_t)(v->cfg_offset + off));
    return n == (ssize_t)len ? 0 : (n < 0 ? -errno : -EIO);
}

int nvm_vfio_cfg_write(struct nvm_vfio *v, uint32_t off, const void *buf, size_t len)
{
    ssize_t n = pwrite(v->device, buf, len, (off_t)(v->cfg_offset + off));
    return n == (ssize_t)len ? 0 : (n < 0 ? -errno : -EIO);
}

int nvm_vfio_enable_bus_master(struct nvm_vfio *v)
{
    /* Without Bus Master Enable the controller cannot fetch SQEs or post CQEs. */
    uint16_t cmd;
    int rc = nvm_vfio_cfg_read(v, PCI_COMMAND, &cmd, sizeof(cmd));
    if (rc)
        return rc;
    cmd |= PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
    return nvm_vfio_cfg_write(v, PCI_COMMAND, &cmd, sizeof(cmd));
}

int nvm_vfio_dma_map(struct nvm_vfio *v, void *va, size_t len, uint64_t iova)
{
    struct vfio_iommu_type1_dma_map m = {
        .argsz = sizeof(m),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = (uint64_t)(uintptr_t)va,
        .iova = iova,
        .size = len,
    };
    if (ioctl(v->container, VFIO_IOMMU_MAP_DMA, &m) < 0) {
        int e = errno;
        nvm_err("VFIO_IOMMU_MAP_DMA iova 0x%llx len %zu: %s", (unsigned long long)iova, len,
                strerror(e));
        return -e;
    }
    return 0;
}

int nvm_vfio_dma_unmap(struct nvm_vfio *v, uint64_t iova, size_t len)
{
    struct vfio_iommu_type1_dma_unmap u = {.argsz = sizeof(u), .iova = iova, .size = len};
    return ioctl(v->container, VFIO_IOMMU_UNMAP_DMA, &u) < 0 ? -errno : 0;
}
