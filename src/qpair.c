#include "qpair.h"
#include "ctrl.h"
#include "dma.h"
#include "mmio.h"
#include "prp.h"
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
                   uint32_t max_xfer, uint64_t timeout_ns)
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
    qp->max_xfer = max_xfer;
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

    /*
     * PRP list pages are preallocated per CID so the submit path never
     * allocates: a CID's list is free again exactly when the CID is.
     */
    qp->prp_pages_per_req = nvm_prp_list_pages(max_xfer);
    if (qp->prp_pages_per_req) {
        size_t per = (size_t)qp->prp_pages_per_req * NVM_PRP_PAGE_SIZE;
        uint64_t iova;
        qp->prp_block = nvm_dma_pool_alloc(pool, per * ncid, &iova);
        if (!qp->prp_block) {
            free(qp->reqs);
            qp->reqs = NULL;
            return -ENOMEM;
        }
        for (uint16_t i = 0; i < ncid; i++) {
            qp->reqs[i].prp_va = (uint64_t *)((char *)qp->prp_block + i * per);
            qp->reqs[i].prp_iova = iova + i * per;
        }
    }
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
    if (qp->prp_block)
        nvm_dma_pool_free(qp->pool, qp->prp_block);
    qp->prp_block = NULL;
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

int nvm_qpair_rw(struct nvm_qpair *qp, uint8_t opc, uint32_t nsid, void *buf, uint64_t lba,
                 uint32_t nlb, uint32_t lba_size, nvm_cb cb, void *arg)
{
    size_t len = (size_t)nlb * lba_size;
    /* NLB is a 0-based 16-bit field (§6.9, §6.15 CDW12). */
    if (NVM_UNLIKELY(nlb == 0 || nlb > 65536 || len > qp->max_xfer))
        return -EINVAL;
    const struct nvm_dma_pool *pool = qp->pool;
    uintptr_t off = (uintptr_t)buf - (uintptr_t)pool->base;
    if (NVM_UNLIKELY(off >= pool->size || pool->size - off < len || (off & (NVM_PAGE_SIZE - 1))))
        return -EINVAL;
    /* Without contiguous IOVAs only a 2 MiB chunk is contiguous on the bus. */
    if (NVM_UNLIKELY(!pool->ops.contiguous &&
                     (off >> NVM_HUGE_SHIFT) != ((off + len - 1) >> NVM_HUGE_SHIFT)))
        return -EINVAL;
    if (NVM_UNLIKELY(nvm_ring_full(qp->sq_head, qp->sq_tail, qp->depth)))
        return -EAGAIN;
    uint16_t cid = nvm_cid_alloc(qp);
    if (NVM_UNLIKELY(cid == NVM_CID_NONE))
        return -EAGAIN;

    struct nvm_req *r = &qp->reqs[cid];
    uint64_t iova = nvm_dma_pool_iova(pool, buf);
    struct nvme_sqe cmd = {
        .opc = opc,
        .cid = cid,
        .nsid = nsid,
        .cdw10 = (uint32_t)lba,
        .cdw11 = (uint32_t)(lba >> 32),
        .cdw12 = nlb - 1,
    };
    struct nvm_prp_page pages[4];
    unsigned np = qp->prp_pages_per_req < 4 ? qp->prp_pages_per_req : 4;
    for (unsigned i = 0; i < np; i++)
        pages[i] = (struct nvm_prp_page){r->prp_va + i * NVM_PRP_PER_PAGE,
                                         r->prp_iova + i * NVM_PRP_PAGE_SIZE};
    int rc = nvm_prp_build(iova, len, pages, np, &cmd.prp1, &cmd.prp2);
    if (NVM_UNLIKELY(rc)) {
        nvm_cid_free(qp, cid);
        return rc;
    }

    r->cb = cb;
    r->cb_arg = arg;
    r->state = NVM_REQ_ACTIVE;
    if (qp->timeout_ns)
        r->submit_ns = nvm_coarse_ns();
    /* Whole-entry store into the ring slot; published later by the doorbell. */
    qp->sq[qp->sq_tail] = cmd;
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

/* ---- Public I/O API ---- */

static uint32_t ns_lba_size(struct nvm_qpair *qp, uint32_t nsid)
{
    if (NVM_LIKELY(qp->ns_nsid == nsid))
        return qp->ns_lba_size;
    const struct nvm_ns_info *ns = qp->ctrl ? nvm_ctrl_ns(qp->ctrl, nsid) : NULL;
    if (!ns)
        return 0;
    qp->ns_nsid = nsid;
    qp->ns_lba_size = ns->lba_size;
    return ns->lba_size;
}

int nvm_read(struct nvm_qpair *qp, uint32_t nsid, void *buf, uint64_t lba, uint32_t nlb, nvm_cb cb,
             void *arg)
{
    uint32_t bs = ns_lba_size(qp, nsid);
    if (NVM_UNLIKELY(!bs))
        return -ENODEV;
    return nvm_qpair_rw(qp, NVME_CMD_READ, nsid, buf, lba, nlb, bs, cb, arg);
}

int nvm_write(struct nvm_qpair *qp, uint32_t nsid, const void *buf, uint64_t lba, uint32_t nlb,
              nvm_cb cb, void *arg)
{
    uint32_t bs = ns_lba_size(qp, nsid);
    if (NVM_UNLIKELY(!bs))
        return -ENODEV;
    /* The device only reads the buffer; the cast drops const for the shared helper. */
    return nvm_qpair_rw(qp, NVME_CMD_WRITE, nsid, (void *)buf, lba, nlb, bs, cb, arg);
}

int nvm_flush(struct nvm_qpair *qp, uint32_t nsid, nvm_cb cb, void *arg)
{
    /* §6.8: commits volatile write cache contents for the namespace. */
    struct nvme_sqe cmd = {.opc = NVME_CMD_FLUSH, .nsid = nsid};
    return nvm_qpair_submit_cmd(qp, &cmd, cb, arg);
}

void nvm_qpair_kick(struct nvm_qpair *qp)
{
    nvm_qpair_kick_internal(qp);
}

int nvm_qpair_poll(struct nvm_qpair *qp, uint32_t max)
{
    return nvm_qpair_poll_internal(qp, max);
}

uint32_t nvm_qpair_outstanding(const struct nvm_qpair *qp)
{
    return qp->outstanding;
}

uint16_t nvm_qpair_id(const struct nvm_qpair *qp)
{
    return qp->qid;
}

void nvm_qpair_get_stats(const struct nvm_qpair *qp, struct nvm_qpair_stats *out)
{
    *out = qp->stats;
}

struct sync_wait {
    int done;
    uint32_t status;
};

static void sync_cb(void *arg, const struct nvm_cpl *cpl)
{
    struct sync_wait *w = arg;
    w->status = cpl->status;
    w->done = 1;
}

static int sync_finish(struct nvm_qpair *qp, int rc, struct sync_wait *w)
{
    if (rc)
        return rc;
    while (!w->done)
        nvm_qpair_poll_internal(qp, qp->depth);
    if (w->status == NVM_STATUS_TIMEOUT)
        return -ETIMEDOUT;
    if (w->status == NVM_STATUS_ABORTED)
        return -ECANCELED;
    return nvm_status_is_error(w->status) ? (int)w->status : 0;
}

int nvm_read_sync(struct nvm_qpair *qp, uint32_t nsid, void *buf, uint64_t lba, uint32_t nlb)
{
    struct sync_wait w = {0};
    return sync_finish(qp, nvm_read(qp, nsid, buf, lba, nlb, sync_cb, &w), &w);
}

int nvm_write_sync(struct nvm_qpair *qp, uint32_t nsid, const void *buf, uint64_t lba, uint32_t nlb)
{
    struct sync_wait w = {0};
    return sync_finish(qp, nvm_write(qp, nsid, buf, lba, nlb, sync_cb, &w), &w);
}

int nvm_flush_sync(struct nvm_qpair *qp, uint32_t nsid)
{
    struct sync_wait w = {0};
    return sync_finish(qp, nvm_flush(qp, nsid, sync_cb, &w), &w);
}
