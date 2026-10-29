/*
 * A fake NVMe controller for host tests: a thread that plays the device over
 * a queue pair's SQ/CQ memory and doorbell words, exactly as a real controller
 * would see them, backed by a RAM disk. It can delay, reorder and fail
 * commands, and it checks the driver's side of the protocol (PRP layout,
 * duplicate CIDs, doorbell values) as it goes.
 *
 * Shared state is accessed with atomics on the device side: the SQ tail
 * doorbell with an acquire load, the CQ head doorbell with an acquire load,
 * and CQE DW3 with a release store. ThreadSanitizer therefore sees the same
 * happens-before edges real hardware relies on, and any race it reports is a
 * real ordering bug in the driver.
 */
#ifndef NVMETAL_FAKECTRL_H
#define NVMETAL_FAKECTRL_H

#include "dma.h"
#include "qpair.h"

#include <stdint.h>

struct fakectrl_opts {
    uint32_t lba_size;     /* default 512 */
    uint64_t nlba;         /* namespace size in LBAs */
    uint32_t window;       /* commands held at once for reordering (1 = in order) */
    uint32_t delay_ppm;    /* probability, per million, of delaying a command */
    uint32_t max_delay_ns; /* upper bound of an injected delay */
    uint32_t error_ppm;    /* probability, per million, of failing a command */
    uint64_t seed;
};

struct fakectrl_stats {
    uint64_t commands;
    uint64_t errors_injected;
    uint64_t reordered;      /* completed while an older command was still held */
    uint64_t prp_violations; /* PRP layout the spec does not allow */
    uint64_t cid_conflicts;  /* CID reused while still outstanding */
    uint64_t bad_doorbells;  /* tail outside the ring */
    uint64_t max_held;
};

struct fakectrl;

/*
 * Allocates SQ/CQ memory for a depth-entry queue pair from pool (which must
 * use identity IOVAs) and fills *mem so nvm_qpair_init() can drive it.
 */
struct fakectrl *fakectrl_create(struct nvm_dma_pool *pool, uint16_t depth,
                                 const struct fakectrl_opts *opts, struct nvm_qpair_mem *mem);
void fakectrl_start(struct fakectrl *f);
void fakectrl_stop(struct fakectrl *f);
void fakectrl_destroy(struct fakectrl *f);
void fakectrl_get_stats(const struct fakectrl *f, struct fakectrl_stats *out);

/* The RAM disk, for tests that want to check contents directly. */
uint8_t *fakectrl_disk(struct fakectrl *f);

/* Commands whose CDW15 equals this are held this long before completing (timeout tests). */
#define FAKECTRL_SLOW_MAGIC 0x510e510eu
void fakectrl_set_slow_ns(struct fakectrl *f, uint64_t ns);

#endif
