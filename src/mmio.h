/*
 * MMIO accessors and DMA memory barriers.
 *
 * Two orderings matter for an NVMe driver (see docs/DESIGN.md, "Memory
 * ordering"):
 *
 *  1. Submission: the 64-byte SQE is written with ordinary stores to DMA
 *     memory, then the SQ tail doorbell is written in BAR0. The device may
 *     fetch the SQE as soon as it sees the doorbell, so every SQE store must be
 *     visible to the device before the doorbell store. nvm_dma_wmb() sits
 *     between the two.
 *
 *  2. Completion: the poller reads CQE DW3, sees the phase tag flip, then reads
 *     DW0/DW2 and the data buffer. Those later loads must not be satisfied
 *     before the phase load, or they could return stale contents from before
 *     the device's write. nvm_dma_rmb() sits after the phase check.
 *
 * x86-64 is TSO: stores leave the core in program order, a store to UC MMIO
 * cannot pass earlier write-back stores, and loads are not reordered with
 * older loads. Both barriers therefore only need to stop the *compiler* from
 * reordering, which an empty asm with a "memory" clobber does.
 *
 * AArch64 is weakly ordered, and the device is an observer in the
 * outer-shareable domain, so a store-release to the doorbell is not enough on
 * its own. `dmb oshst` orders all earlier stores before later stores for
 * outer-shareable observers, which is what Linux's dma_wmb()/__iowmb() uses
 * before writel(). `dsb st` would also work but stalls until the stores
 * complete, which is stronger than needed when the next access is itself a
 * store. On the read side `dmb oshld` orders the phase load before later
 * loads.
 *
 * nvm_dma_mb() orders earlier loads *and* stores before later ones. It guards
 * the CQ head doorbell: the CQE loads must finish before the store that lets
 * the device reuse those slots, and `dmb oshst` would not order loads.
 *
 * The doorbell store is additionally a C11 release store and the DW3 load an
 * acquire load. On x86-64 these compile to plain MOVs (no extra cost), but
 * they tell the compiler, and ThreadSanitizer when the fake controller plays
 * the device in tests, about the same ordering the hardware barriers provide.
 */
#ifndef NVMETAL_MMIO_H
#define NVMETAL_MMIO_H

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(__i386__)
#define nvm_dma_wmb() __asm__ __volatile__("" ::: "memory")
#define nvm_dma_rmb() __asm__ __volatile__("" ::: "memory")
#define nvm_dma_mb()  __asm__ __volatile__("" ::: "memory")
#elif defined(__aarch64__)
#define nvm_dma_wmb() __asm__ __volatile__("dmb oshst" ::: "memory")
#define nvm_dma_rmb() __asm__ __volatile__("dmb oshld" ::: "memory")
#define nvm_dma_mb()  __asm__ __volatile__("dmb osh" ::: "memory")
#else
/* Unknown architecture: a full fence is correct if slower. */
#define nvm_dma_wmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define nvm_dma_rmb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define nvm_dma_mb()  __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

/*
 * 32-bit MMIO accesses. The NVMe registers are little endian; nvmetal only
 * targets little-endian hosts (checked below), so no byte swapping is needed.
 */
static inline uint32_t nvm_mmio_read32(const volatile void *base, size_t off)
{
    const volatile uint32_t *p = (const volatile uint32_t *)((const volatile char *)base + off);
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void nvm_mmio_write32(volatile void *base, size_t off, uint32_t v)
{
    volatile uint32_t *p = (volatile uint32_t *)((volatile char *)base + off);
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

/*
 * 64-bit registers (CAP, ASQ, ACQ) are accessed as two 32-bit halves, low
 * dword first. §3.1 permits that and it avoids relying on the platform
 * supporting 8-byte MMIO transactions.
 */
static inline uint64_t nvm_mmio_read64(const volatile void *base, size_t off)
{
    uint64_t lo = nvm_mmio_read32(base, off);
    uint64_t hi = nvm_mmio_read32(base, off + 4);
    return lo | hi << 32;
}

static inline void nvm_mmio_write64(volatile void *base, size_t off, uint64_t v)
{
    nvm_mmio_write32(base, off, (uint32_t)v);
    nvm_mmio_write32(base, off + 4, (uint32_t)(v >> 32));
}

/* Doorbell write: barrier, then a release store (see the header comment). */
static inline void nvm_doorbell_write(volatile uint32_t *db, uint32_t v)
{
    nvm_dma_wmb();
    __atomic_store_n(db, v, __ATOMIC_RELEASE);
}

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "nvmetal assumes a little-endian host: NVMe structures are little endian"
#endif

#endif
