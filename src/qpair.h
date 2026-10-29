/*
 * Queue pair internals: one submission queue and one completion queue, owned
 * by a single thread. Also used for the admin queue (QID 0).
 */
#ifndef NVMETAL_QPAIR_H
#define NVMETAL_QPAIR_H

#include "nvme_spec.h"
#include "nvmetal/nvmetal.h"

#include <stdint.h>

struct nvm_dma_pool;
struct nvm_ctrl;

/* ---- Ring index math (§4.1 "Submission Queue & Completion Queue Definition") ---- */

static inline uint16_t nvm_ring_next(uint16_t idx, uint16_t depth)
{
    return (uint16_t)(idx + 1 == depth ? 0 : idx + 1);
}

/* Entries between head and tail. */
static inline uint16_t nvm_ring_used(uint16_t head, uint16_t tail, uint16_t depth)
{
    return (uint16_t)(tail >= head ? tail - head : depth - head + tail);
}

/* §4.1.2: a queue is full when tail + 1 == head, so one slot always stays empty. */
static inline int nvm_ring_full(uint16_t head, uint16_t tail, uint16_t depth)
{
    return nvm_ring_next(tail, depth) == head;
}

enum nvm_req_state {
    NVM_REQ_FREE = 0,
    NVM_REQ_ACTIVE = 1,
    /* Completed to the caller with NVM_STATUS_TIMEOUT; the CID stays reserved
     * until the device's late completion arrives, so it cannot be confused
     * with a new command reusing the CID. */
    NVM_REQ_TIMED_OUT = 2,
};

struct nvm_req {
    nvm_cb cb;
    void *cb_arg;
    uint64_t submit_ns; /* coarse clock, for timeouts only */
    uint16_t state;
    uint16_t next_free;
    /* This CID's PRP list pages (prp_pages_per_req of them, contiguous). */
    uint64_t *prp_va;
    uint64_t prp_iova;
};

/* Memory and doorbells for a queue pair, provided by the controller or a test. */
struct nvm_qpair_mem {
    uint16_t qid;
    uint16_t depth; /* entries in each of SQ and CQ */
    struct nvme_sqe *sq;
    uint64_t sq_iova;
    struct nvme_cqe *cq;
    uint64_t cq_iova;
    volatile uint32_t *sq_db;
    volatile uint32_t *cq_db;
};

struct nvm_qpair {
    /* Hot path state, touched on every submit/poll. */
    struct nvme_sqe *sq;
    struct nvme_cqe *cq;
    volatile uint32_t *sq_db;
    volatile uint32_t *cq_db;
    struct nvm_req *reqs;
    uint16_t depth;
    uint16_t sq_tail;    /* next free SQ slot */
    uint16_t sq_db_tail; /* tail value last written to the doorbell */
    uint16_t sq_head;    /* from the most recent CQE's SQHD */
    uint16_t cq_head;
    uint8_t phase;      /* expected phase tag of the next new CQE */
    uint16_t free_head; /* CID free list, linked through nvm_req.next_free */
    uint16_t nfree;
    uint16_t outstanding; /* commands the caller is still waiting for */
    uint32_t idle_polls;
    uint64_t timeout_ns;
    uint32_t max_xfer;
    unsigned prp_pages_per_req;
    int irq_fd;       /* eventfd of this CQ's MSI-X vector, -1 when polled */
    uint32_t ns_nsid; /* one-entry namespace cache for the public read/write calls */
    uint32_t ns_lba_size;

    uint16_t qid;
    uint64_t sq_iova;
    uint64_t cq_iova;
    void *prp_block;
    struct nvm_dma_pool *pool;
    struct nvm_ctrl *ctrl;
    struct nvm_qpair_stats stats;
};

#define NVM_CID_NONE 0xffff

/*
 * Sets up ring state over caller-provided memory; CIDs 0..depth-2 are usable.
 * Each CID gets enough PRP list pages from the pool for max_xfer bytes.
 */
int nvm_qpair_init(struct nvm_qpair *qp, const struct nvm_qpair_mem *mem, struct nvm_dma_pool *pool,
                   uint32_t max_xfer, uint64_t timeout_ns);
void nvm_qpair_fini(struct nvm_qpair *qp);

/* CID allocator: returns NVM_CID_NONE when every CID is in use. */
static inline uint16_t nvm_cid_alloc(struct nvm_qpair *qp)
{
    uint16_t cid = qp->free_head;
    if (cid == NVM_CID_NONE)
        return cid;
    qp->free_head = qp->reqs[cid].next_free;
    qp->nfree--;
    return cid;
}

static inline void nvm_cid_free(struct nvm_qpair *qp, uint16_t cid)
{
    qp->reqs[cid].state = NVM_REQ_FREE;
    qp->reqs[cid].next_free = qp->free_head;
    qp->free_head = cid;
    qp->nfree++;
}

/*
 * Queue a fully formed command (the CID field is filled in here). Does not
 * ring the doorbell: call nvm_qpair_kick(), or nvm_qpair_poll() which kicks
 * first. Returns -EAGAIN when the queue is full.
 */
int nvm_qpair_submit_cmd(struct nvm_qpair *qp, const struct nvme_sqe *cmd, nvm_cb cb, void *arg);

/*
 * Queue a read or write of nlb blocks of lba_size bytes. buf must be DMA pool
 * memory, 4 KiB aligned. Does not ring the doorbell. Returns -EINVAL for bad
 * arguments, -EAGAIN when the queue is full.
 */
int nvm_qpair_rw(struct nvm_qpair *qp, uint8_t opc, uint32_t nsid, void *buf, uint64_t lba,
                 uint32_t nlb, uint32_t lba_size, nvm_cb cb, void *arg);

/* Writes the SQ tail doorbell once for everything queued since the last kick. */
void nvm_qpair_kick_internal(struct nvm_qpair *qp);

/* Processes up to max completions; returns how many. */
int nvm_qpair_poll_internal(struct nvm_qpair *qp, uint32_t max);

/* Times out commands older than timeout_ns. Returns how many timed out. */
int nvm_qpair_check_timeouts(struct nvm_qpair *qp, uint64_t now_ns);

/* Coarse monotonic clock (jiffy resolution): cheap enough to read per I/O. */
uint64_t nvm_coarse_ns(void);

#endif
