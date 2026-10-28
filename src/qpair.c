#include "qpair.h"
#include "mmio.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* How many empty polls between timeout scans: keeps the clock read off the
 * common path while still noticing a stuck command within microseconds of
 * spinning. */
#define NVM_TIMEOUT_SCAN_INTERVAL 4096

uint64_t nvm_coarse_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int nvm_qpair_init(struct nvm_qpair *qp, const struct nvm_qpair_mem *mem, struct nvm_dma_pool *pool,
                   uint64_t timeout_ns)
{
    if (mem->depth < 2)
        return -EINVAL;
    memset(qp, 0, sizeof(*qp));
    qp->sq = mem->sq;
    qp->cq = mem->cq;
    qp->sq_db = mem->sq_db;
    qp->cq_db = mem->cq_db;
    qp->sq_iova = mem->sq_iova;
    qp->cq_iova = mem->cq_iova;
    qp->depth = mem->depth;
    qp->qid = mem->qid;
    qp->pool = pool;
    qp->timeout_ns = timeout_ns;
    /*
     * The CQ memory is zeroed, so every entry's phase tag reads 0 and the
     * device writes its first pass with phase 1 (§4.6, "Phase Tag").
     */
    qp->phase = 1;

    /*
     * depth - 1 CIDs: at most depth - 1 commands can sit in the SQ (§4.1.2),
     * and capping outstanding commands at the same number means the CQ (also
     * depth entries) can never overflow either.
     */
    uint16_t ncid = (uint16_t)(qp->depth - 1);
    qp->reqs = calloc(ncid, sizeof(*qp->reqs));
    if (!qp->reqs)
        return -ENOMEM;
    qp->free_head = NVM_CID_NONE;
    for (int i = ncid - 1; i >= 0; i--)
        nvm_cid_free(qp, (uint16_t)i);
    return 0;
}

void nvm_qpair_fini(struct nvm_qpair *qp)
{
    /* Anything still outstanding will never complete now: tell the callers. */
    uint16_t ncid = (uint16_t)(qp->depth - 1);
    for (uint16_t cid = 0; qp->reqs && cid < ncid; cid++) {
        struct nvm_req *r = &qp->reqs[cid];
        if (r->state == NVM_REQ_ACTIVE && r->cb) {
            struct nvm_cpl cpl = {.status = NVM_STATUS_ABORTED, .cid = cid};
            r->cb(r->cb_arg, &cpl);
        }
    }
    free(qp->reqs);
    qp->reqs = NULL;
}

int nvm_qpair_submit_cmd(struct nvm_qpair *qp, const struct nvme_sqe *cmd, nvm_cb cb, void *arg)
{
    if (NVM_UNLIKELY(nvm_ring_full(qp->sq_head, qp->sq_tail, qp->depth)))
        return -EAGAIN;
    uint16_t cid = nvm_cid_alloc(qp);
    if (NVM_UNLIKELY(cid == NVM_CID_NONE))
        return -EAGAIN;

    struct nvm_req *r = &qp->reqs[cid];
    r->cb = cb;
    r->cb_arg = arg;
    r->state = NVM_REQ_ACTIVE;
    if (qp->timeout_ns)
        r->submit_ns = nvm_coarse_ns();

    struct nvme_sqe *slot = &qp->sq[qp->sq_tail];
    *slot = *cmd;
    slot->cid = cid;
    qp->sq_tail = nvm_ring_next(qp->sq_tail, qp->depth);
    qp->outstanding++;
    qp->stats.submitted++;
    return 0;
}

void nvm_qpair_kick_internal(struct nvm_qpair *qp)
{
    if (qp->sq_tail == qp->sq_db_tail)
        return;
    /* One doorbell write publishes every SQE queued since the last one; the
     * barrier inside nvm_doorbell_write orders those SQE stores first. */
    nvm_doorbell_write(qp->sq_db, qp->sq_tail);
    qp->sq_db_tail = qp->sq_tail;
    qp->stats.doorbells++;
}

static void complete_one(struct nvm_qpair *qp, uint16_t cid, uint16_t sf, uint32_t dw0)
{
    if (NVM_UNLIKELY(cid >= qp->depth - 1)) {
        nvm_err("qid %u: completion for invalid CID %u", qp->qid, cid);
        return;
    }
    struct nvm_req *r = &qp->reqs[cid];
    if (NVM_UNLIKELY(r->state != NVM_REQ_ACTIVE)) {
        if (r->state == NVM_REQ_TIMED_OUT) {
            /* The device finished a command we already gave up on: only now
             * is its CID safe to reuse. */
            qp->stats.late_completions++;
            nvm_cid_free(qp, cid);
        } else {
            nvm_err("qid %u: completion for idle CID %u", qp->qid, cid);
        }
        return;
    }
    nvm_cb cb = r->cb;
    void *arg = r->cb_arg;
    /* Free the CID before the callback so the callback can resubmit. */
    nvm_cid_free(qp, cid);
    qp->outstanding--;
    qp->stats.completed++;
    if (cb) {
        struct nvm_cpl cpl = {.status = sf, .dw0 = dw0, .cid = cid};
        cb(arg, &cpl);
    }
}

int nvm_qpair_poll_internal(struct nvm_qpair *qp, uint32_t max)
{
    nvm_qpair_kick_internal(qp);

    uint32_t n = 0;
    while (n < max) {
        struct nvme_cqe *cqe = &qp->cq[qp->cq_head];
        /* Acquire load of DW3 (CID, phase, status in one access). */
        uint32_t dw3 = __atomic_load_n(&cqe->dw3, __ATOMIC_ACQUIRE);
        if (NVME_CQE_PHASE(dw3) != qp->phase)
            break;
        /* The phase matched: only now may the rest of the entry (and the
         * data the command transferred) be read. See mmio.h. */
        nvm_dma_rmb();
        uint32_t dw0 = cqe->dw0;
        qp->sq_head = cqe->sqhd;

        /* Advance before the callback so a re-entrant poll sees a consistent ring. */
        qp->cq_head = nvm_ring_next(qp->cq_head, qp->depth);
        if (qp->cq_head == 0)
            qp->phase ^= 1;
        n++;
        complete_one(qp, NVME_CQE_CID(dw3), NVME_CQE_STATUS(dw3), dw0);
    }

    if (n) {
        /*
         * One CQ head doorbell for the whole batch hands the slots back to the
         * device. The CQE loads above must be done before that store, or the
         * device could overwrite an entry still being read: nvm_dma_mb orders
         * loads and stores (a compiler barrier on x86, dmb osh on arm64).
         */
        nvm_dma_mb();
        __atomic_store_n(qp->cq_db, qp->cq_head, __ATOMIC_RELEASE);
        qp->idle_polls = 0;
    } else if (NVM_UNLIKELY(qp->timeout_ns && qp->outstanding &&
                            ++qp->idle_polls >= NVM_TIMEOUT_SCAN_INTERVAL)) {
        qp->idle_polls = 0;
        nvm_qpair_check_timeouts(qp, nvm_coarse_ns());
    }
    return (int)n;
}

int nvm_qpair_check_timeouts(struct nvm_qpair *qp, uint64_t now_ns)
{
    int n = 0;
    uint16_t ncid = (uint16_t)(qp->depth - 1);
    for (uint16_t cid = 0; cid < ncid; cid++) {
        struct nvm_req *r = &qp->reqs[cid];
        if (r->state != NVM_REQ_ACTIVE || now_ns - r->submit_ns < qp->timeout_ns)
            continue;
        r->state = NVM_REQ_TIMED_OUT;
        qp->outstanding--;
        qp->stats.timeouts++;
        n++;
        nvm_warn("qid %u: CID %u timed out after %llu ms", qp->qid, cid,
                 (unsigned long long)((now_ns - r->submit_ns) / 1000000));
        if (r->cb) {
            struct nvm_cpl cpl = {.status = NVM_STATUS_TIMEOUT, .cid = cid};
            r->cb(r->cb_arg, &cpl);
        }
    }
    return n;
}
