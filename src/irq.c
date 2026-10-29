/*
 * MSI-X interrupts via VFIO (Documentation/driver-api/vfio.rst, "VFIO
 * Interrupts"; include/uapi/linux/vfio.h, VFIO_DEVICE_SET_IRQS). The NVMe side
 * is §7.5.2 "MSI-X Interrupts": each I/O CQ names its vector in Create I/O CQ
 * CDW11.IV, and the controller signals that vector when it posts to the CQ.
 */
#include "irq.h"
#include "util.h"
#include "vfio.h"

#include <errno.h>
#include <linux/vfio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int nvm_irq_msix_enable(struct nvm_vfio *v, const int *fds, uint32_t n)
{
    size_t sz = sizeof(struct vfio_irq_set) + n * sizeof(int32_t);
    struct vfio_irq_set *s = calloc(1, sz);
    if (!s)
        return -ENOMEM;
    s->argsz = (uint32_t)sz;
    s->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    s->index = VFIO_PCI_MSIX_IRQ_INDEX;
    s->start = 0;
    s->count = n;
    memcpy(s->data, fds, n * sizeof(int32_t));
    int rc = ioctl(v->device, VFIO_DEVICE_SET_IRQS, s) < 0 ? -errno : 0;
    free(s);
    if (rc)
        nvm_err("%s: enabling %u MSI-X vectors: %s", v->bdf, n, strerror(-rc));
    return rc;
}

int nvm_irq_msix_disable(struct nvm_vfio *v)
{
    struct vfio_irq_set s = {
        .argsz = sizeof(s),
        .flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
        .index = VFIO_PCI_MSIX_IRQ_INDEX,
        .start = 0,
        .count = 0,
    };
    return ioctl(v->device, VFIO_DEVICE_SET_IRQS, &s) < 0 ? -errno : 0;
}

int nvm_irq_wait(int fd, int timeout_ms)
{
    uint64_t cnt;
    if (timeout_ms >= 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int rc = poll(&p, 1, timeout_ms);
        if (rc <= 0)
            return rc < 0 ? -errno : 0;
    }
    /* The eventfd counts interrupts; one read consumes them all. */
    ssize_t n = read(fd, &cnt, sizeof(cnt));
    if (n != sizeof(cnt))
        return n < 0 ? -errno : -EIO;
    return 1;
}

void nvm_irq_drain(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    uint64_t cnt;
    while (poll(&p, 1, 0) > 0 && read(fd, &cnt, sizeof(cnt)) == sizeof(cnt))
        ;
}
