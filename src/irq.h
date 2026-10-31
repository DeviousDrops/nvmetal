/* MSI-X through VFIO: one eventfd per vector, signalled by the kernel's vfio-pci handler. */
#ifndef NVMETAL_IRQ_H
#define NVMETAL_IRQ_H

#include <stdint.h>

struct nvm_vfio;

/* Routes MSI-X vectors 0..n-1 to fds[0..n-1] (VFIO_DEVICE_SET_IRQS, ACTION_TRIGGER). */
int nvm_irq_msix_enable(struct nvm_vfio *v, const int *fds, uint32_t n);

/* Disables MSI-X for the device. */
int nvm_irq_msix_disable(struct nvm_vfio *v);

/*
 * Blocks until the eventfd is signalled or timeout_ms passes (-1 = forever).
 * Returns 1 when signalled, 0 on timeout, -errno on error. With no timeout
 * this is a single blocking read(): one syscall per wakeup.
 */
int nvm_irq_wait(int fd, int timeout_ms);

/* Clears any pending count without blocking. */
void nvm_irq_drain(int fd);

#endif
