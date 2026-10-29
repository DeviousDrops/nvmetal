/*
 * The real queue pair code against the fake controller: millions of reads,
 * writes and flushes with random sizes (PRP1 only up to chained-free lists),
 * random delays, out-of-order completions and injected errors, checking every
 * byte read back. Run under ASan+UBSan and, separately, TSan.
 *
 * NVMETAL_FAKE_CMDS scales the command counts (default 1000000 for the single
 * queue pair, the same split over four threads for the multi-threaded case).
 */
#include "fakectrl.h"
#include "nvme_spec.h"
#include "qpair.h"
#include "test.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>

#define LBA_SIZE    512u
#define REGION_LBAS 256u /* per in-flight slot: 128 KiB */
#define MAX_XFER    (64u * 1024)

static uint64_t total_cmds(void)
{
    const char *e = getenv("NVMETAL_FAKE_CMDS");
    return e ? strtoull(e, NULL, 0) : 1000000;
}

static uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

struct client;

struct slot {
    struct client *cl;
    uint8_t *buf;
    uint64_t region; /* first LBA owned by this slot */
    uint64_t lba;
    uint32_t nlb;
    uint32_t seq;
    int op; /* 0 read, 1 write, 2 flush */
};

struct client {
    struct nvm_dma_pool *pool;
    struct nvm_qpair qp;
    struct fakectrl *f;
    struct slot *slots;
    int *free_slots;
    int nfree;
    uint16_t depth;
    uint32_t *expect; /* per LBA: sequence of the last successful write, 0 = never */
    uint32_t seq;
    uint64_t rng;
    uint64_t completed, errors, mismatches, bad_dw0, bytes;
};

static uint64_t crand(struct client *c)
{
    c->rng = mix(c->rng);
    return c->rng;
}

static void fill(uint8_t *p, uint64_t lba, uint32_t seq)
{
    for (unsigned i = 0; i < LBA_SIZE; i += 8) {
        uint64_t v = mix(lba << 20 ^ (uint64_t)seq << 8 ^ i);
        memcpy(p + i, &v, 8);
    }
}

static int matches(const uint8_t *p, uint64_t lba, uint32_t seq)
{
    if (seq == 0) {
        for (unsigned i = 0; i < LBA_SIZE; i++)
            if (p[i])
                return 0;
        return 1;
    }
    uint8_t want[LBA_SIZE];
    fill(want, lba, seq);
    return memcmp(p, want, LBA_SIZE) == 0;
}

static void done_cb(void *arg, const struct nvm_cpl *cpl)
{
    struct slot *s = arg;
    struct client *c = s->cl;
    c->completed++;
    if (cpl->dw0 != (cpl->cid ^ 0x5a5a0000u))
        c->bad_dw0++;
    if (nvm_status_is_error(cpl->status)) {
        /* The fake applies no data on failure, so expectations stay put. */
        c->errors++;
    } else if (s->op == 1) {
        for (uint32_t i = 0; i < s->nlb; i++)
            c->expect[s->lba + i] = s->seq;
        c->bytes += (uint64_t)s->nlb * LBA_SIZE;
    } else if (s->op == 0) {
        for (uint32_t i = 0; i < s->nlb; i++)
            if (!matches(s->buf + (size_t)i * LBA_SIZE, s->lba + i, c->expect[s->lba + i]))
                c->mismatches++;
        c->bytes += (uint64_t)s->nlb * LBA_SIZE;
    }
    c->free_slots[c->nfree++] = (int)(s - c->slots);
}

static void client_init(struct client *c, struct nvm_dma_pool *pool, uint16_t depth,
                        const struct fakectrl_opts *fo_in, uint64_t timeout_ns)
{
    memset(c, 0, sizeof(*c));
    c->pool = pool;
    c->depth = depth;
    c->rng = fo_in->seed * 7 + 3;
    int nslots = depth - 1;
    struct fakectrl_opts fo = *fo_in;
    fo.nlba = (uint64_t)nslots * REGION_LBAS;
    struct nvm_qpair_mem mem;
    c->f = fakectrl_create(pool, depth, &fo, &mem);
    REQUIRE(nvm_qpair_init(&c->qp, &mem, pool, MAX_XFER, timeout_ns) == 0);
    c->slots = calloc((size_t)nslots, sizeof(*c->slots));
    c->free_slots = calloc((size_t)nslots, sizeof(int));
    c->expect = calloc(fo.nlba, sizeof(uint32_t));
    for (int i = 0; i < nslots; i++) {
        c->slots[i] = (struct slot){.cl = c, .region = (uint64_t)i * REGION_LBAS};
        c->slots[i].buf = nvm_dma_pool_alloc(pool, MAX_XFER, NULL);
        REQUIRE(c->slots[i].buf);
        c->free_slots[c->nfree++] = i;
    }
    fakectrl_start(c->f);
}

static void client_fini(struct client *c)
{
    fakectrl_stop(c->f);
    nvm_qpair_fini(&c->qp);
    for (int i = 0; i < c->depth - 1; i++)
        nvm_dma_pool_free(c->pool, c->slots[i].buf);
    fakectrl_destroy(c->f);
    free(c->slots);
    free(c->free_slots);
    free(c->expect);
}

static uint32_t pick_nlb(struct client *c)
{
    uint64_t r = crand(c) % 100;
    uint32_t max = MAX_XFER / LBA_SIZE;
    if (r < 70)
        return 1 + (uint32_t)(crand(c) % 8); /* within one page */
    if (r < 95)
        return 9 + (uint32_t)(crand(c) % 24);      /* PRP2 or a short list */
    return 33 + (uint32_t)(crand(c) % (max - 32)); /* long PRP list */
}

/* Keeps the queue full until n commands have completed. */
static void client_run(struct client *c, uint64_t n)
{
    uint64_t submitted = 0;
    uint64_t target = c->completed + n;
    while (c->completed < target) {
        int queued = 0;
        while (submitted < n && c->nfree > 0) {
            struct slot *s = &c->slots[c->free_slots[c->nfree - 1]];
            uint64_t r = crand(c) % 100;
            int rc;
            if (r < 1) {
                s->op = 2;
                s->nlb = 0;
                rc = nvm_qpair_submit_cmd(
                    &c->qp, &(struct nvme_sqe){.opc = NVME_CMD_FLUSH, .nsid = 1}, done_cb, s);
            } else {
                s->nlb = pick_nlb(c);
                s->lba = s->region + crand(c) % (REGION_LBAS - s->nlb + 1);
                s->op = r < 50 ? 1 : 0;
                if (s->op == 1) {
                    s->seq = ++c->seq;
                    for (uint32_t i = 0; i < s->nlb; i++)
                        fill(s->buf + (size_t)i * LBA_SIZE, s->lba + i, s->seq);
                }
                rc = nvm_qpair_rw(&c->qp, s->op ? NVME_CMD_WRITE : NVME_CMD_READ, 1, s->buf, s->lba,
                                  s->nlb, LBA_SIZE, done_cb, s);
            }
            if (rc == -EAGAIN)
                break;
            REQUIRE(rc == 0);
            c->nfree--;
            submitted++;
            queued++;
        }
        /* Ring once per batch, sometimes several batches per ring. */
        if (queued && (crand(c) & 3))
            nvm_qpair_kick_internal(&c->qp);
        if (nvm_qpair_poll_internal(&c->qp, c->depth) == 0)
            sched_yield();
    }
}

static void check_clean(struct client *c, const char *what)
{
    struct fakectrl_stats st;
    fakectrl_get_stats(c->f, &st);
    printf("  %s: %llu cmds, %llu MiB, %llu injected errors, %llu reordered, max held %llu, "
           "%llu doorbells\n",
           what, (unsigned long long)st.commands, (unsigned long long)(c->bytes >> 20),
           (unsigned long long)st.errors_injected, (unsigned long long)st.reordered,
           (unsigned long long)st.max_held, (unsigned long long)c->qp.stats.doorbells);
    CHECK_EQ(c->mismatches, 0);
    CHECK_EQ(c->bad_dw0, 0);
    CHECK_EQ(st.prp_violations, 0);
    CHECK_EQ(st.cid_conflicts, 0);
    CHECK_EQ(st.bad_doorbells, 0);
    CHECK_EQ(c->errors, st.errors_injected);
    CHECK_EQ(c->qp.outstanding, 0);
}

static void make_pool(struct nvm_dma_pool *p, size_t mib)
{
    struct nvm_iova_ops ops;
    nvm_iova_ops_identity(&ops);
    REQUIRE(nvm_dma_pool_init(p, mib << 20, 0, &ops) == 0);
}

TEST(stress_single_qpair)
{
    struct nvm_dma_pool pool;
    make_pool(&pool, 16);
    struct fakectrl_opts fo = {
        .window = 16, .delay_ppm = 50000, .max_delay_ns = 20000, .error_ppm = 10000, .seed = 11};
    struct client c;
    client_init(&c, &pool, 64, &fo, 0);
    client_run(&c, total_cmds());
    struct fakectrl_stats st;
    fakectrl_get_stats(c.f, &st);
    check_clean(&c, "1 qpair, depth 64");
    CHECK(st.reordered > 0);
    CHECK(st.errors_injected > 0);
    CHECK(c.completed >= total_cmds());
    client_fini(&c);
    nvm_dma_pool_fini(&pool);
}

static void *mt_worker(void *arg)
{
    client_run(arg, total_cmds() / 4);
    return NULL;
}

TEST(stress_four_threads_own_qpairs)
{
    /* One pool shared by all, one queue pair and one fake device per thread:
     * the I/O path must need no locking for this to be race free. */
    struct nvm_dma_pool pool;
    make_pool(&pool, 32);
    struct client c[4];
    pthread_t t[4];
    for (int i = 0; i < 4; i++) {
        struct fakectrl_opts fo = {.window = 8,
                                   .delay_ppm = 20000,
                                   .max_delay_ns = 10000,
                                   .error_ppm = 5000,
                                   .seed = 100 + (uint64_t)i};
        client_init(&c[i], &pool, 32, &fo, 0);
    }
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, mt_worker, &c[i]);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    for (int i = 0; i < 4; i++) {
        check_clean(&c[i], "thread");
        client_fini(&c[i]);
    }
    nvm_dma_pool_fini(&pool);
}

TEST(depth_two_in_order)
{
    /* The smallest legal queue: one usable entry, so every command wraps. */
    struct nvm_dma_pool pool;
    make_pool(&pool, 4);
    struct fakectrl_opts fo = {.window = 1, .seed = 5};
    struct client c;
    client_init(&c, &pool, 2, &fo, 0);
    client_run(&c, total_cmds() / 10);
    check_clean(&c, "depth 2");
    client_fini(&c);
    nvm_dma_pool_fini(&pool);
}

struct flag {
    int n;
    uint32_t status;
};

static void flag_cb(void *arg, const struct nvm_cpl *cpl)
{
    struct flag *f = arg;
    f->n++;
    f->status = cpl->status;
}

TEST(timeout_then_late_completion)
{
    struct nvm_dma_pool pool;
    make_pool(&pool, 4);
    struct fakectrl_opts fo = {.window = 4, .seed = 9};
    struct client c;
    /* Coarse-clock timestamps can be a jiffy off at either end, so the timeout
     * is far above any normal latency and the slow command far above it. */
    client_init(&c, &pool, 8, &fo, 100 * 1000000ull);
    fakectrl_set_slow_ns(c.f, 1000 * 1000000ull);
    struct flag slow = {0}, fast = {0};
    struct nvme_sqe cmd = {.opc = NVME_CMD_FLUSH, .nsid = 1, .cdw15 = FAKECTRL_SLOW_MAGIC};
    REQUIRE(nvm_qpair_submit_cmd(&c.qp, &cmd, flag_cb, &slow) == 0);
    REQUIRE(nvm_qpair_submit_cmd(&c.qp, &(struct nvme_sqe){.opc = NVME_CMD_FLUSH, .nsid = 1},
                                 flag_cb, &fast) == 0);
    while (slow.n == 0)
        nvm_qpair_poll_internal(&c.qp, 8);
    CHECK_EQ(slow.status, NVM_STATUS_TIMEOUT);
    CHECK_EQ(fast.n, 1);
    CHECK_EQ(fast.status, 0);
    CHECK_EQ(c.qp.outstanding, 0);
    CHECK_EQ(c.qp.nfree, 6); /* the timed-out CID is still held */
    /* Normal traffic keeps flowing while the slow command is pending. */
    client_run(&c, 2000);
    /* Wait for the device's late answer: it must be absorbed silently. */
    while (c.qp.stats.late_completions == 0)
        nvm_qpair_poll_internal(&c.qp, 8);
    CHECK_EQ(slow.n, 1);
    CHECK_EQ(c.qp.nfree, 7);
    check_clean(&c, "timeout");
    client_fini(&c);
    nvm_dma_pool_fini(&pool);
}

TEST(lba_out_of_range_reported)
{
    struct nvm_dma_pool pool;
    make_pool(&pool, 4);
    struct fakectrl_opts fo = {.window = 1, .seed = 3};
    struct client c;
    client_init(&c, &pool, 4, &fo, 0);
    struct flag fl = {0};
    uint64_t nlba = 3 * REGION_LBAS;
    REQUIRE(nvm_qpair_rw(&c.qp, NVME_CMD_READ, 1, c.slots[0].buf, nlba - 1, 2, LBA_SIZE, flag_cb,
                         &fl) == 0);
    while (!fl.n)
        nvm_qpair_poll_internal(&c.qp, 4);
    CHECK_EQ(NVM_STATUS_SC(fl.status), NVME_SC_LBA_RANGE);
    CHECK_EQ(NVM_STATUS_DNR(fl.status), 1);
    fakectrl_stop(c.f);
    nvm_qpair_fini(&c.qp);
    for (int i = 0; i < 3; i++)
        nvm_dma_pool_free(&pool, c.slots[i].buf);
    fakectrl_destroy(c.f);
    free(c.slots);
    free(c.free_slots);
    free(c.expect);
    nvm_dma_pool_fini(&pool);
}

TEST(teardown_aborts_outstanding)
{
    struct nvm_dma_pool pool;
    make_pool(&pool, 4);
    struct fakectrl_opts fo = {.window = 4, .seed = 4};
    struct client c;
    client_init(&c, &pool, 8, &fo, 0);
    fakectrl_set_slow_ns(c.f, 10ull * 1000000000ull);
    struct flag fl[3] = {{0}};
    for (int i = 0; i < 3; i++)
        REQUIRE(
            nvm_qpair_submit_cmd(
                &c.qp,
                &(struct nvme_sqe){.opc = NVME_CMD_FLUSH, .nsid = 1, .cdw15 = FAKECTRL_SLOW_MAGIC},
                flag_cb, &fl[i]) == 0);
    nvm_qpair_kick_internal(&c.qp);
    fakectrl_stop(c.f);
    nvm_qpair_fini(&c.qp);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(fl[i].n, 1);
        CHECK_EQ(fl[i].status, NVM_STATUS_ABORTED);
    }
    for (int i = 0; i < 7; i++)
        nvm_dma_pool_free(&pool, c.slots[i].buf);
    fakectrl_destroy(c.f);
    free(c.slots);
    free(c.free_slots);
    free(c.expect);
    nvm_dma_pool_fini(&pool);
}

TEST_MAIN()
