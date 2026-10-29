#include "fakectrl.h"
#include "nvme_spec.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_WINDOW 64

struct held {
    struct nvme_sqe sqe;
    uint64_t ready_ns;
    uint64_t order; /* fetch order, to count reordering */
};

struct fakectrl {
    struct nvm_dma_pool *pool;
    struct fakectrl_opts o;
    uint16_t depth;
    struct nvme_sqe *sq;
    struct nvme_cqe *cq;
    uint32_t sq_db; /* doorbell "registers" */
    uint32_t cq_db;
    uint8_t *disk;

    /* device-thread state */
    uint16_t sq_head;
    uint16_t cq_tail;
    uint8_t phase;
    struct held held[MAX_WINDOW];
    uint32_t nheld;
    uint64_t fetched;
    uint64_t rng;
    uint8_t *cid_busy;
    uint64_t slow_ns;

    pthread_t thread;
    int stop;
    struct fakectrl_stats st;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t rnd(struct fakectrl *f)
{
    f->rng += 0x9e3779b97f4a7c15ull;
    uint64_t x = f->rng;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

static int chance(struct fakectrl *f, uint32_t ppm)
{
    return ppm && rnd(f) % 1000000 < ppm;
}

/*
 * Walks the PRPs of a command the way a controller does (§4.3) and checks
 * the layout. Fills pages[] with host pointers (IOVA == VA in tests).
 */
static int walk_prps(struct fakectrl *f, const struct nvme_sqe *s, size_t len, uint8_t **pages,
                     size_t max_pages)
{
    size_t n = (len + 4095) / 4096;
    if (n > max_pages || (s->prp1 & 4095)) /* nvmetal never sends an offset */
        return -1;
    pages[0] = (uint8_t *)(uintptr_t)s->prp1;
    if (n == 1)
        return s->prp2 == 0 ? 0 : -1; /* driver leaves PRP2 zero; flag noise */
    if (n == 2) {
        pages[1] = (uint8_t *)(uintptr_t)s->prp2;
        return (s->prp2 & 4095) ? -1 : 0;
    }
    if (s->prp2 & 7)
        return -1;
    const uint64_t *list = (const uint64_t *)(uintptr_t)s->prp2;
    size_t idx = ((uintptr_t)list & 4095) / 8, i = 1;
    while (i < n) {
        if (idx == 511 && n - i > 1) {
            /* Last entry of a list page with more to come: chain pointer. */
            uint64_t next = list[idx];
            if (next & 4095)
                return -1;
            list = (const uint64_t *)(uintptr_t)next;
            idx = 0;
            continue;
        }
        uint64_t e = list[idx++];
        if (e & 4095)
            return -1;
        pages[i++] = (uint8_t *)(uintptr_t)e;
    }
    (void)f;
    return 0;
}

static uint16_t execute(struct fakectrl *f, const struct nvme_sqe *s)
{
    if (chance(f, f->o.error_ppm)) {
        f->st.errors_injected++;
        /* A mix of generic, media and DNR-flagged failures. */
        switch (rnd(f) % 3) {
        case 0:
            return NVME_SC_INTERNAL;
        case 1:
            return (uint16_t)(NVME_SCT_MEDIA << 8 | NVME_SC_READ_ERROR);
        default:
            return (uint16_t)(1u << 14 | NVME_SCT_MEDIA << 8 | NVME_SC_WRITE_FAULT);
        }
    }
    if (s->opc == NVME_CMD_FLUSH)
        return 0;
    if (s->opc != NVME_CMD_READ && s->opc != NVME_CMD_WRITE)
        return NVME_SC_INVALID_OPCODE;
    if (s->nsid != 1)
        return NVME_SC_INVALID_NS;
    uint64_t slba = (uint64_t)s->cdw11 << 32 | s->cdw10;
    uint32_t nlb = (s->cdw12 & 0xffff) + 1;
    if (slba >= f->o.nlba || f->o.nlba - slba < nlb)
        return (uint16_t)(1u << 14 | NVME_SC_LBA_RANGE);
    size_t len = (size_t)nlb * f->o.lba_size;
    uint8_t *pages[1024];
    if (walk_prps(f, s, len, pages, 1024)) {
        f->st.prp_violations++;
        return NVME_SC_PRP_OFFSET_INVALID;
    }
    uint8_t *d = f->disk + slba * f->o.lba_size;
    for (size_t i = 0, off = 0; off < len; i++, off += 4096) {
        size_t chunk = len - off < 4096 ? len - off : 4096;
        if (s->opc == NVME_CMD_WRITE)
            memcpy(d + off, pages[i], chunk);
        else
            memcpy(pages[i], d + off, chunk);
    }
    return 0;
}

static void post(struct fakectrl *f, uint16_t cid, uint16_t sf)
{
    /* §4.6: never fill the last free slot; wait for the host to consume. */
    while (nvm_ring_next(f->cq_tail, f->depth) == __atomic_load_n(&f->cq_db, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE))
            return;
        sched_yield();
    }
    struct nvme_cqe *e = &f->cq[f->cq_tail];
    e->dw0 = cid ^ 0x5a5a0000u;
    e->dw1 = 0;
    e->sqhd = f->sq_head;
    e->sqid = 1;
    /* DW3 last, as a release store: the phase flip publishes the entry and
     * everything the command wrote into host memory before it. */
    __atomic_store_n(&e->dw3, cid | (uint32_t)f->phase << 16 | (uint32_t)sf << 17,
                     __ATOMIC_RELEASE);
    f->cq_tail = nvm_ring_next(f->cq_tail, f->depth);
    if (f->cq_tail == 0)
        f->phase ^= 1;
}

static void *device_loop(void *arg)
{
    struct fakectrl *f = arg;
    uint32_t idle = 0;
    while (!__atomic_load_n(&f->stop, __ATOMIC_ACQUIRE)) {
        int did = 0;
        uint32_t tail = __atomic_load_n(&f->sq_db, __ATOMIC_ACQUIRE);
        if (tail >= f->depth) {
            f->st.bad_doorbells++;
            tail = f->sq_head;
        }
        uint64_t now = now_ns();
        /* Fetch while there is room in the reorder window. */
        while (f->sq_head != tail && f->nheld < f->o.window) {
            struct held *h = &f->held[f->nheld++];
            h->sqe = f->sq[f->sq_head];
            h->order = f->fetched++;
            h->ready_ns = now;
            if (h->sqe.cdw15 == FAKECTRL_SLOW_MAGIC)
                h->ready_ns += __atomic_load_n(&f->slow_ns, __ATOMIC_RELAXED);
            else if (chance(f, f->o.delay_ppm))
                h->ready_ns += rnd(f) % (f->o.max_delay_ns + 1);
            if (f->cid_busy[h->sqe.cid])
                f->st.cid_conflicts++;
            f->cid_busy[h->sqe.cid] = 1;
            f->sq_head = nvm_ring_next(f->sq_head, f->depth);
            did = 1;
        }
        if (f->nheld > f->st.max_held)
            f->st.max_held = f->nheld;
        /* Complete one random ready command. */
        if (f->nheld) {
            uint32_t start = (uint32_t)(rnd(f) % f->nheld), pick = UINT32_MAX;
            for (uint32_t k = 0; k < f->nheld; k++) {
                uint32_t i = (start + k) % f->nheld;
                if (f->held[i].ready_ns <= now) {
                    pick = i;
                    break;
                }
            }
            if (pick != UINT32_MAX) {
                struct held h = f->held[pick];
                for (uint32_t i = 0; i < f->nheld; i++)
                    if (f->held[i].order < h.order) {
                        f->st.reordered++;
                        break;
                    }
                f->held[pick] = f->held[--f->nheld];
                uint16_t sf = execute(f, &h.sqe);
                f->cid_busy[h.sqe.cid] = 0;
                f->st.commands++;
                post(f, h.sqe.cid, sf);
                did = 1;
            }
        }
        if (!did && ++idle > 64) {
            idle = 0;
            sched_yield();
        }
    }
    return NULL;
}

struct fakectrl *fakectrl_create(struct nvm_dma_pool *pool, uint16_t depth,
                                 const struct fakectrl_opts *opts, struct nvm_qpair_mem *mem)
{
    struct fakectrl *f = calloc(1, sizeof(*f));
    f->pool = pool;
    f->o = *opts;
    if (!f->o.lba_size)
        f->o.lba_size = 512;
    if (!f->o.window || f->o.window > MAX_WINDOW)
        f->o.window = f->o.window ? MAX_WINDOW : 1;
    f->depth = depth;
    f->phase = 1;
    f->rng = opts->seed ? opts->seed : 1;
    f->disk = calloc(f->o.nlba, f->o.lba_size);
    f->cid_busy = calloc(65536, 1);
    uint64_t sq_iova, cq_iova;
    f->sq = nvm_dma_pool_alloc(pool, depth * sizeof(struct nvme_sqe), &sq_iova);
    f->cq = nvm_dma_pool_alloc(pool, depth * sizeof(struct nvme_cqe), &cq_iova);
    if (!f->disk || !f->cid_busy || !f->sq || !f->cq)
        abort();
    *mem = (struct nvm_qpair_mem){
        .qid = 1,
        .depth = depth,
        .sq = f->sq,
        .sq_iova = sq_iova,
        .cq = f->cq,
        .cq_iova = cq_iova,
        .sq_db = &f->sq_db,
        .cq_db = &f->cq_db,
    };
    return f;
}

void fakectrl_start(struct fakectrl *f)
{
    pthread_create(&f->thread, NULL, device_loop, f);
}

void fakectrl_stop(struct fakectrl *f)
{
    __atomic_store_n(&f->stop, 1, __ATOMIC_RELEASE);
    pthread_join(f->thread, NULL);
}

void fakectrl_destroy(struct fakectrl *f)
{
    nvm_dma_pool_free(f->pool, f->sq);
    nvm_dma_pool_free(f->pool, f->cq);
    free(f->disk);
    free(f->cid_busy);
    free(f);
}

void fakectrl_get_stats(const struct fakectrl *f, struct fakectrl_stats *out)
{
    *out = f->st;
}

uint8_t *fakectrl_disk(struct fakectrl *f)
{
    return f->disk;
}

void fakectrl_set_slow_ns(struct fakectrl *f, uint64_t ns)
{
    __atomic_store_n(&f->slow_ns, ns, __ATOMIC_RELAXED);
}
