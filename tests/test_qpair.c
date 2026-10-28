/*
 * Queue pair core on host memory: the test itself plays the device, one
 * entry at a time, so every ring position, wrap and phase flip is checked
 * deterministically.
 */
#include "dma.h"
#include "nvme_spec.h"
#include "qpair.h"
#include "test.h"

#include <errno.h>

struct rig {
    struct nvm_dma_pool pool;
    struct nvm_qpair qp;
    uint32_t sq_db, cq_db; /* stand-ins for the BAR0 doorbells */
    uint16_t dev_sq_head;  /* device side state */
    uint16_t dev_cq_tail;
    uint8_t dev_phase;
};

static void rig_init(struct rig *r, uint16_t depth, uint64_t timeout_ns)
{
    memset(r, 0, sizeof(*r));
    struct nvm_iova_ops ops;
    nvm_iova_ops_identity(&ops);
    REQUIRE(nvm_dma_pool_init(&r->pool, 2u << 20, 0, &ops) == 0);
    struct nvm_qpair_mem m = {.qid = 1, .depth = depth};
    m.sq = nvm_dma_pool_alloc(&r->pool, depth * sizeof(struct nvme_sqe), &m.sq_iova);
    m.cq = nvm_dma_pool_alloc(&r->pool, depth * sizeof(struct nvme_cqe), &m.cq_iova);
    m.sq_db = &r->sq_db;
    m.cq_db = &r->cq_db;
    REQUIRE(m.sq && m.cq);
    REQUIRE(nvm_qpair_init(&r->qp, &m, &r->pool, timeout_ns) == 0);
    r->dev_phase = 1;
}

static void rig_fini(struct rig *r)
{
    nvm_qpair_fini(&r->qp);
    nvm_dma_pool_fini(&r->pool);
}

/* Device: fetch the next SQE (must be published by the doorbell). */
static struct nvme_sqe dev_fetch(struct rig *r)
{
    REQUIRE(r->dev_sq_head != r->sq_db);
    struct nvme_sqe s = r->qp.sq[r->dev_sq_head];
    r->dev_sq_head = nvm_ring_next(r->dev_sq_head, r->qp.depth);
    return s;
}

/* Device: post a CQE for cid with status field sf. */
static void dev_post(struct rig *r, uint16_t cid, uint16_t sf, uint32_t dw0)
{
    REQUIRE(!nvm_ring_full((uint16_t)r->cq_db, r->dev_cq_tail, r->qp.depth));
    struct nvme_cqe *e = &r->qp.cq[r->dev_cq_tail];
    e->dw0 = dw0;
    e->sqhd = r->dev_sq_head;
    e->sqid = 1;
    e->dw3 = cid | (uint32_t)r->dev_phase << 16 | (uint32_t)sf << 17;
    r->dev_cq_tail = nvm_ring_next(r->dev_cq_tail, r->qp.depth);
    if (r->dev_cq_tail == 0)
        r->dev_phase ^= 1;
}

struct seen {
    int n;
    uint32_t status[64];
    uint32_t dw0[64];
    uintptr_t tag[64];
};

static void record_cb(void *arg, const struct nvm_cpl *cpl)
{
    struct seen *s = arg;
    if (s->n < 64) {
        s->status[s->n] = cpl->status;
        s->dw0[s->n] = cpl->dw0;
    }
    s->n++;
}

TEST(ring_math)
{
    CHECK_EQ(nvm_ring_next(0, 4), 1);
    CHECK_EQ(nvm_ring_next(3, 4), 0);
    CHECK_EQ(nvm_ring_used(0, 0, 4), 0);
    CHECK_EQ(nvm_ring_used(1, 3, 4), 2);
    CHECK_EQ(nvm_ring_used(3, 1, 4), 2); /* wrapped */
    CHECK_EQ(nvm_ring_used(2, 1, 4), 3);
    CHECK(!nvm_ring_full(0, 2, 4));
    CHECK(nvm_ring_full(0, 3, 4));
    CHECK(nvm_ring_full(2, 1, 4));
    CHECK(!nvm_ring_full(1, 1, 4)); /* empty */
    /* Depth 2 holds exactly one entry. */
    CHECK(nvm_ring_full(0, 1, 2));
    CHECK(nvm_ring_full(1, 0, 2));
}

TEST(cid_allocator_exhaustion_and_reuse)
{
    struct rig r;
    rig_init(&r, 8, 0);
    CHECK_EQ(r.qp.nfree, 7);
    uint8_t used[7] = {0};
    for (int i = 0; i < 7; i++) {
        uint16_t c = nvm_cid_alloc(&r.qp);
        REQUIRE(c < 7);
        CHECK(!used[c]);
        used[c] = 1;
    }
    CHECK_EQ(nvm_cid_alloc(&r.qp), NVM_CID_NONE);
    CHECK_EQ(r.qp.nfree, 0);
    nvm_cid_free(&r.qp, 5);
    nvm_cid_free(&r.qp, 2);
    /* LIFO: the most recently freed CID comes back first (warm cache line). */
    CHECK_EQ(nvm_cid_alloc(&r.qp), 2);
    CHECK_EQ(nvm_cid_alloc(&r.qp), 5);
    CHECK_EQ(nvm_cid_alloc(&r.qp), NVM_CID_NONE);
    for (uint16_t c = 0; c < 7; c++)
        nvm_cid_free(&r.qp, c);
    CHECK_EQ(r.qp.nfree, 7);
    rig_fini(&r);
}

TEST(submit_does_not_ring_until_kick)
{
    struct rig r;
    rig_init(&r, 16, 0);
    struct nvme_sqe cmd = {.opc = NVME_CMD_READ, .nsid = 1};
    for (int i = 0; i < 3; i++)
        CHECK_EQ(nvm_qpair_submit_cmd(&r.qp, &cmd, NULL, NULL), 0);
    CHECK_EQ(r.sq_db, 0);
    nvm_qpair_kick_internal(&r.qp);
    CHECK_EQ(r.sq_db, 3);
    CHECK_EQ(r.qp.stats.doorbells, 1);
    nvm_qpair_kick_internal(&r.qp); /* nothing new: no second write */
    CHECK_EQ(r.qp.stats.doorbells, 1);
    rig_fini(&r);
}

TEST(wraparound_and_phase_flips)
{
    /* 10 laps of a depth-4 ring, one command at a time, then 3 at a time. */
    struct rig r;
    rig_init(&r, 4, 0);
    struct seen s = {0};
    for (int i = 0; i < 40; i++) {
        struct nvme_sqe cmd = {.opc = NVME_CMD_WRITE, .cdw10 = (uint32_t)i};
        REQUIRE(nvm_qpair_submit_cmd(&r.qp, &cmd, record_cb, &s) == 0);
        nvm_qpair_kick_internal(&r.qp);
        CHECK_EQ(r.sq_db, (uint32_t)((i + 1) % 4));
        struct nvme_sqe got = dev_fetch(&r);
        CHECK_EQ(got.cdw10, (uint32_t)i);
        CHECK_EQ(got.opc, NVME_CMD_WRITE);
        /* Nothing completes before the device posts. */
        CHECK_EQ(nvm_qpair_poll_internal(&r.qp, 8), 0);
        dev_post(&r, got.cid, 0, (uint32_t)i);
        CHECK_EQ(nvm_qpair_poll_internal(&r.qp, 8), 1);
        CHECK_EQ(r.cq_db, (uint32_t)((i + 1) % 4));
        CHECK_EQ(s.n, i + 1);
    }
    CHECK_EQ(r.qp.phase, 1 ^ (40 / 4 % 2)); /* flipped once per lap */
    for (int lap = 0; lap < 20; lap++) {
        uint16_t cids[3];
        for (int i = 0; i < 3; i++) {
            struct nvme_sqe cmd = {.opc = NVME_CMD_READ};
            REQUIRE(nvm_qpair_submit_cmd(&r.qp, &cmd, record_cb, &s) == 0);
        }
        /* Depth 4: the fourth submission must be refused. */
        struct nvme_sqe extra = {.opc = NVME_CMD_READ};
        CHECK_EQ(nvm_qpair_submit_cmd(&r.qp, &extra, NULL, NULL), -EAGAIN);
        nvm_qpair_kick_internal(&r.qp);
        for (int i = 0; i < 3; i++)
            cids[i] = dev_fetch(&r).cid;
        for (int i = 0; i < 3; i++)
            dev_post(&r, cids[i], 0, 0);
        CHECK_EQ(nvm_qpair_poll_internal(&r.qp, 64), 3);
    }
    CHECK_EQ(s.n, 40 + 60);
    CHECK_EQ(r.qp.outstanding, 0);
    CHECK_EQ(r.qp.nfree, 3);
    rig_fini(&r);
}

TEST(out_of_order_completion_and_status)
{
    struct rig r;
    rig_init(&r, 16, 0);
    struct seen s[5] = {0};
    for (int i = 0; i < 5; i++) {
        struct nvme_sqe cmd = {.opc = NVME_CMD_READ};
        REQUIRE(nvm_qpair_submit_cmd(&r.qp, &cmd, record_cb, &s[i]) == 0);
    }
    nvm_qpair_kick_internal(&r.qp);
    uint16_t cid[5];
    for (int i = 0; i < 5; i++)
        cid[i] = dev_fetch(&r).cid;
    /* Complete in reverse; give #2 an LBA Out of Range error with DNR. */
    for (int i = 4; i >= 0; i--) {
        uint16_t sf = i == 2 ? (uint16_t)(1u << 14 | NVME_SC_LBA_RANGE) : 0;
        dev_post(&r, cid[i], sf, 100u + (uint32_t)i);
    }
    CHECK_EQ(nvm_qpair_poll_internal(&r.qp, 2), 2); /* max is honoured */
    CHECK_EQ(nvm_qpair_poll_internal(&r.qp, 64), 3);
    for (int i = 0; i < 5; i++) {
        CHECK_EQ(s[i].n, 1);
        CHECK_EQ(s[i].dw0[0], 100u + (uint32_t)i);
    }
    CHECK(nvm_status_is_error(s[2].status[0]));
    CHECK_EQ(NVM_STATUS_SC(s[2].status[0]), NVME_SC_LBA_RANGE);
    CHECK_EQ(NVM_STATUS_SCT(s[2].status[0]), NVME_SCT_GENERIC);
    CHECK_EQ(NVM_STATUS_DNR(s[2].status[0]), 1);
    CHECK(!nvm_status_is_error(s[0].status[0]));
    CHECK_EQ(r.qp.sq_head, 5); /* SQHD from the CQEs */
    rig_fini(&r);
}

TEST(timeout_then_late_completion)
{
    struct rig r;
    rig_init(&r, 8, 1000000 /* 1 ms */);
    struct seen s = {0};
    struct nvme_sqe cmd = {.opc = NVME_CMD_READ};
    REQUIRE(nvm_qpair_submit_cmd(&r.qp, &cmd, record_cb, &s) == 0);
    nvm_qpair_kick_internal(&r.qp);
    uint16_t cid = dev_fetch(&r).cid;
    uint64_t t0 = r.qp.reqs[cid].submit_ns;
    CHECK_EQ(nvm_qpair_check_timeouts(&r.qp, t0 + 500000), 0);
    CHECK_EQ(nvm_qpair_check_timeouts(&r.qp, t0 + 2000000), 1);
    CHECK_EQ(s.n, 1);
    CHECK_EQ(s.status[0], NVM_STATUS_TIMEOUT);
    CHECK_EQ(r.qp.outstanding, 0);
    /* The CID must stay reserved until the device answers. */
    CHECK_EQ(r.qp.nfree, 6);
    CHECK_EQ(nvm_qpair_check_timeouts(&r.qp, t0 + 9000000), 0); /* not twice */
    dev_post(&r, cid, 0, 0);
    CHECK_EQ(nvm_qpair_poll_internal(&r.qp, 8), 1);
    CHECK_EQ(s.n, 1); /* the caller is not called again */
    CHECK_EQ(r.qp.stats.late_completions, 1);
    CHECK_EQ(r.qp.nfree, 7);
    rig_fini(&r);
}

struct resubmit_ctx {
    struct nvm_qpair *qp;
    int left;
    int done;
};

static void resubmit_cb(void *arg, const struct nvm_cpl *cpl)
{
    struct resubmit_ctx *c = arg;
    (void)cpl;
    c->done++;
    if (c->left-- > 0) {
        struct nvme_sqe cmd = {.opc = NVME_CMD_READ};
        /* Depth 2 has one usable slot: this only works because the CID and
         * SQ slot are released before the callback runs. */
        CHECK_EQ(nvm_qpair_submit_cmd(c->qp, &cmd, resubmit_cb, c), 0);
    }
}

TEST(callback_can_resubmit)
{
    struct rig r;
    rig_init(&r, 2, 0);
    struct resubmit_ctx c = {&r.qp, 4, 0};
    struct nvme_sqe cmd = {.opc = NVME_CMD_READ};
    REQUIRE(nvm_qpair_submit_cmd(&r.qp, &cmd, resubmit_cb, &c) == 0);
    for (int i = 0; i < 5; i++) {
        nvm_qpair_kick_internal(&r.qp);
        dev_post(&r, dev_fetch(&r).cid, 0, 0);
        CHECK_EQ(nvm_qpair_poll_internal(&r.qp, 8), 1);
    }
    CHECK_EQ(c.done, 5);
    CHECK_EQ(r.qp.outstanding, 0);
    rig_fini(&r);
}

TEST(status_strings)
{
    char b[96];
    CHECK(strstr(nvm_status_str(NVME_SC_LBA_RANGE, b, sizeof(b)), "LBA Out of Range"));
    CHECK(strstr(nvm_status_str(1u << 8 | NVME_SC_QID_INVALID, b, sizeof(b)),
                 "Invalid Queue Identifier"));
    CHECK(strstr(nvm_status_str(2u << 8 | NVME_SC_READ_ERROR, b, sizeof(b)),
                 "Unrecovered Read Error"));
    CHECK(strstr(nvm_status_str(1u << 14 | NVME_SC_INVALID_FIELD, b, sizeof(b)), "dnr"));
    CHECK(strstr(nvm_status_str(NVM_STATUS_TIMEOUT, b, sizeof(b)), "Timeout"));
    CHECK(strstr(nvm_status_str(0x7fe, b, sizeof(b)), "Vendor Specific"));
    CHECK(strstr(nvm_status_str(0x0ee, b, sizeof(b)), "Unknown"));
}

TEST_MAIN()
