/*
 * Controller bring-up and teardown: reset, admin queue, enable, Identify,
 * queue negotiation, I/O queue creation/deletion and normal shutdown. The
 * order follows §7.6.1 "Initialization" and §7.6.2 "Shutdown".
 */
#include "ctrl.h"
#include "irq.h"
#include "mmio.h"
#include "nvme_spec.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <string.h>
#include <unistd.h>

#define NVM_ADMIN_DEPTH_DEFAULT 64
#define NVM_ADMIN_TIMEOUT_NS    (10ull * 1000000000ull)
#define NVM_IO_TIMEOUT_NS       (30ull * 1000000000ull)
#define NVM_POOL_DEFAULT        (128u << 20)
#define NVM_DRIVER_MAX_XFER     (2u << 20)
#define NVM_IO_DEPTH_DEFAULT    256
/* Queues requested from Set Features (Number of Queues); the controller may grant fewer. */
#define NVM_QUEUES_WANTED NVM_MAX_QID

static uint32_t csts(const struct nvm_ctrl *c)
{
    return nvm_mmio_read32(c->regs, NVME_REG_CSTS);
}

/*
 * Waits for CSTS.RDY to equal want, for at most CAP.TO (§3.1.1: "the worst
 * case time that host software shall wait for CSTS.RDY to transition").
 */
static int wait_ready(struct nvm_ctrl *c, int want)
{
    uint64_t deadline = nvm_now_ns() + (uint64_t)c->timeout_ms * 1000000ull;
    for (;;) {
        uint32_t s = csts(c);
        if (s == UINT32_MAX) {
            nvm_err("%s: CSTS reads all ones: device not responding", c->info.bdf);
            return -ENODEV;
        }
        /* §3.1.6: CFS while enabling means the controller hit a fatal error;
         * while disabling it is expected and clearing EN is the cure. */
        if (want && (s & NVME_CSTS_CFS)) {
            nvm_err("%s: controller fatal status during enable", c->info.bdf);
            return -EIO;
        }
        if (!!(s & NVME_CSTS_RDY) == want)
            return 0;
        if (nvm_now_ns() > deadline) {
            nvm_err("%s: timeout waiting for CSTS.RDY=%d (CAP.TO %u ms)", c->info.bdf, want,
                    c->timeout_ms);
            return -ETIMEDOUT;
        }
        usleep(1000);
    }
}

/* Controller Level Reset by clearing CC.EN (§7.3.2), then wait for RDY=0. */
static int ctrl_disable(struct nvm_ctrl *c)
{
    uint32_t cc = nvm_mmio_read32(c->regs, NVME_REG_CC);
    uint32_t s = csts(c);
    if (s & NVME_CSTS_CFS)
        nvm_warn("%s: controller fatal status set, resetting", c->info.bdf);
    if (cc & NVME_CC_EN) {
        /* §3.1.5: EN must not go 1 -> 0 while RDY is still 0 from the
         * previous enable; let that transition finish first. */
        if (!(s & NVME_CSTS_RDY) && !(s & NVME_CSTS_CFS)) {
            int rc = wait_ready(c, 1);
            if (rc && rc != -EIO)
                return rc;
        }
        nvm_mmio_write32(c->regs, NVME_REG_CC, cc & ~NVME_CC_EN);
    }
    return wait_ready(c, 0);
}

static int ctrl_enable(struct nvm_ctrl *c)
{
    /*
     * CC per §3.1.5: NVM command set, 4 KiB memory pages (MPS = 0, checked
     * against CAP.MPSMIN at open), round-robin arbitration, 64-byte SQEs
     * (IOSQES = 6) and 16-byte CQEs (IOCQES = 4). Written once without EN,
     * then again with EN, so the configuration is in place before the
     * controller starts using it.
     */
    uint32_t cc =
        NVME_CC_CSS_NVM | NVME_CC_MPS(0) | NVME_CC_AMS_RR | NVME_CC_IOSQES(6) | NVME_CC_IOCQES(4);
    nvm_mmio_write32(c->regs, NVME_REG_CC, cc);
    nvm_mmio_write32(c->regs, NVME_REG_CC, cc | NVME_CC_EN);
    return wait_ready(c, 1);
}

struct sync_wait {
    volatile int done;
    struct nvm_cpl cpl;
};

static void sync_cb(void *arg, const struct nvm_cpl *cpl)
{
    struct sync_wait *w = arg;
    w->cpl = *cpl;
    w->done = 1;
}

int nvm_admin_sync(struct nvm_ctrl *c, struct nvme_sqe *cmd, void *buf, size_t len,
                   struct nvm_cpl *out)
{
    if (buf) {
        /* Admin payloads here are at most two pages: PRP1 + PRP2 (§4.3). */
        if (len > 2 * NVM_PAGE_SIZE || ((uintptr_t)buf & (NVM_PAGE_SIZE - 1)))
            return -EINVAL;
        cmd->prp1 = nvm_dma_pool_iova(&c->pool, buf);
        if (cmd->prp1 == UINT64_MAX)
            return -EINVAL;
        cmd->prp2 = len > NVM_PAGE_SIZE ? cmd->prp1 + NVM_PAGE_SIZE : 0;
    }
    struct sync_wait w = {0};
    pthread_mutex_lock(&c->admin_lock);
    int rc = nvm_qpair_submit_cmd(&c->admin, cmd, sync_cb, &w);
    if (rc == 0) {
        while (!w.done)
            nvm_qpair_poll_internal(&c->admin, 1);
    }
    pthread_mutex_unlock(&c->admin_lock);
    if (rc)
        return rc;
    if (out)
        *out = w.cpl;
    if (w.cpl.status == NVM_STATUS_TIMEOUT) {
        if (csts(c) & NVME_CSTS_CFS)
            nvm_err("%s: controller fatal status set", c->info.bdf);
        return -ETIMEDOUT;
    }
    if (nvm_status_is_error(w.cpl.status)) {
        char sbuf[96];
        nvm_debug("admin opcode 0x%02x failed: %s", cmd->opc,
                  nvm_status_str(w.cpl.status, sbuf, sizeof(sbuf)));
        return (int)w.cpl.status;
    }
    return 0;
}

/* Identify strings are space padded, not NUL terminated (§5.15.2). */
static void copy_trimmed(char *dst, const char *src, size_t n)
{
    memcpy(dst, src, n);
    dst[n] = '\0';
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0'))
        dst[--n] = '\0';
}

static int identify(struct nvm_ctrl *c, uint8_t cns, uint32_t nsid)
{
    struct nvme_sqe cmd = {.opc = NVME_ADMIN_IDENTIFY, .nsid = nsid, .cdw10 = cns};
    return nvm_admin_sync(c, &cmd, c->admin_buf, NVM_PAGE_SIZE, NULL);
}

static int identify_controller(struct nvm_ctrl *c)
{
    int rc = identify(c, NVME_CNS_CTRL, 0);
    if (rc)
        return rc;
    const struct nvme_id_ctrl *id = c->admin_buf;
    struct nvm_ctrl_info *in = &c->info;
    copy_trimmed(in->model, id->mn, sizeof(id->mn));
    copy_trimmed(in->serial, id->sn, sizeof(id->sn));
    copy_trimmed(in->firmware, id->fr, sizeof(id->fr));
    in->vid = id->vid;
    in->ssvid = id->ssvid;
    in->oacs = id->oacs;
    in->oncs = id->oncs;
    in->vwc = id->vwc;
    in->nn = id->nn;
    /* MDTS is a power of two in units of the minimum memory page size (§5.15.2). */
    uint32_t mps_min = NVM_PAGE_SIZE << NVME_CAP_MPSMIN(c->cap);
    in->mdts_bytes = id->mdts ? (id->mdts >= 20 ? UINT32_MAX : mps_min << id->mdts) : 0;
    in->max_xfer = NVM_DRIVER_MAX_XFER;
    if (in->mdts_bytes && in->mdts_bytes < in->max_xfer)
        in->max_xfer = in->mdts_bytes;
    /* §5.15.2: SQES/CQES bits 3:0 are the required (minimum) entry sizes. */
    if ((id->sqes & 0xf) > 6 || (id->cqes & 0xf) > 4) {
        nvm_err("controller requires larger queue entries than 64/16 bytes");
        return -ENOTSUP;
    }
    return 0;
}

static int identify_namespaces(struct nvm_ctrl *c)
{
    /* Active Namespace ID list (CNS 02h): up to 1024 NSIDs in ascending order. */
    int rc = identify(c, NVME_CNS_ACTIVE_NSID, 0);
    if (rc)
        return rc;
    uint32_t ids[1024];
    memcpy(ids, c->admin_buf, sizeof(ids));
    uint32_t n = 0;
    while (n < 1024 && ids[n])
        n++;
    c->ns = calloc(n ? n : 1, sizeof(*c->ns));
    if (!c->ns)
        return -ENOMEM;
    for (uint32_t i = 0; i < n; i++) {
        if ((rc = identify(c, NVME_CNS_NS, ids[i])) != 0)
            return rc;
        const struct nvme_id_ns *id = c->admin_buf;
        struct nvm_ns_info *ns = &c->ns[c->nns];
        const struct nvme_lbaf *f = &id->lbaf[id->flbas & 0xf];
        ns->nsid = ids[i];
        ns->nlba = id->nsze;
        ns->ncap = id->ncap;
        ns->nuse = id->nuse;
        ns->nlbaf = id->nlbaf;
        ns->flbas = id->flbas;
        ns->lba_size = f->lbads >= 9 && f->lbads < 32 ? 1u << f->lbads : 0;
        ns->ms = f->ms;
        if (!ns->lba_size || ns->nlba == 0) {
            nvm_warn("namespace %u: unusable format (lbads %u, nsze %llu), skipped", ids[i],
                     f->lbads, (unsigned long long)id->nsze);
            continue;
        }
        c->nns++;
    }
    c->info.num_ns = c->nns;
    return 0;
}

static int negotiate_queues(struct nvm_ctrl *c)
{
    /* §5.21.1.7: NSQR in CDW11 15:0, NCQR in 31:16, both 0-based; the
     * allocation comes back in the same format in DW0. */
    struct nvme_sqe cmd = {
        .opc = NVME_ADMIN_SET_FEATURES,
        .cdw10 = NVME_FEAT_NUM_QUEUES,
        .cdw11 = (uint32_t)(NVM_QUEUES_WANTED - 1) << 16 | (NVM_QUEUES_WANTED - 1),
    };
    struct nvm_cpl cpl;
    int rc = nvm_admin_sync(c, &cmd, NULL, 0, &cpl);
    if (rc)
        return rc;
    uint32_t nsq = (cpl.dw0 & 0xffff) + 1, ncq = (cpl.dw0 >> 16) + 1;
    uint32_t n = nsq < ncq ? nsq : ncq;
    c->info.max_qpairs = (uint16_t)(n > NVM_MAX_QID ? NVM_MAX_QID : n);
    return 0;
}

static size_t pool_bytes(const struct nvm_ctrl_opts *o)
{
    if (o && o->dma_pool_bytes)
        return o->dma_pool_bytes;
    const char *e = getenv("NVMETAL_POOL_MB");
    if (e && atoi(e) > 0)
        return (size_t)atoi(e) << 20;
    return NVM_POOL_DEFAULT;
}

int nvm_ctrl_open(const char *bdf, const struct nvm_ctrl_opts *opts, struct nvm_ctrl **out)
{
    struct nvm_ctrl *c = calloc(1, sizeof(*c));
    if (!c)
        return -ENOMEM;
    pthread_mutex_init(&c->admin_lock, NULL);
    pthread_mutex_init(&c->qid_lock, NULL);
    for (int i = 0; i <= NVM_MAX_QID; i++)
        c->irq_fds[i] = -1;
    snprintf(c->info.bdf, sizeof(c->info.bdf), "%s", bdf);

    int rc = nvm_vfio_open(&c->vfio, bdf);
    if (rc)
        goto fail_free;
    enum nvm_iova_mode mode = opts ? opts->iova_mode : NVM_IOVA_AUTO;
    if ((mode == NVM_IOVA_IOMMU && c->vfio.noiommu) ||
        (mode == NVM_IOVA_PHYS && !c->vfio.noiommu)) {
        nvm_err("%s: requested IOVA mode does not match the VFIO group (%s)", bdf,
                c->vfio.noiommu ? "no-IOMMU" : "IOMMU");
        rc = -EINVAL;
        goto fail_vfio;
    }
    c->info.iommu = !c->vfio.noiommu;
    c->info.msix_vectors = c->vfio.msix_count;
    c->regs = c->vfio.bar0;

    if ((rc = nvm_vfio_enable_bus_master(&c->vfio)) != 0)
        goto fail_vfio;

    struct nvm_iova_ops ops;
    if (c->vfio.noiommu)
        nvm_iova_ops_phys(&ops);
    else
        nvm_iova_ops_vfio(&ops, &c->iova_ctx, &c->vfio);
    if ((rc = nvm_dma_pool_init(&c->pool, pool_bytes(opts), 1, &ops)) != 0)
        goto fail_vfio;

    c->cap = nvm_mmio_read64(c->regs, NVME_REG_CAP);
    c->info.cap = c->cap;
    c->info.version = nvm_mmio_read32(c->regs, NVME_REG_VS);
    c->dstrd = NVME_CAP_DSTRD(c->cap);
    c->timeout_ms = NVME_CAP_TO(c->cap) * 500u;
    if (c->timeout_ms == 0)
        c->timeout_ms = 500;
    c->info.max_queue_entries = NVME_CAP_MQES(c->cap) + 1;
    if (!(NVME_CAP_CSS(c->cap) & NVME_CAP_CSS_NVM) || NVME_CAP_MPSMIN(c->cap) != 0) {
        nvm_err("%s: needs NVM command set and 4 KiB pages (CAP 0x%llx)", bdf,
                (unsigned long long)c->cap);
        rc = -ENOTSUP;
        goto fail_pool;
    }

    if ((rc = ctrl_disable(c)) != 0)
        goto fail_pool;

    /* Admin queue (§7.6.1 steps: configure AQA/ASQ/ACQ while disabled). */
    uint32_t adepth = opts && opts->admin_depth ? opts->admin_depth : NVM_ADMIN_DEPTH_DEFAULT;
    if (adepth < 2 || adepth > 4096 || adepth > c->info.max_queue_entries) {
        rc = -EINVAL;
        goto fail_pool;
    }
    uint64_t asq_iova, acq_iova;
    c->asq = nvm_dma_pool_alloc(&c->pool, adepth * sizeof(struct nvme_sqe), &asq_iova);
    c->acq = nvm_dma_pool_alloc(&c->pool, adepth * sizeof(struct nvme_cqe), &acq_iova);
    c->admin_buf = nvm_dma_pool_alloc(&c->pool, NVM_PAGE_SIZE, &c->admin_buf_iova);
    if (!c->asq || !c->acq || !c->admin_buf) {
        rc = -ENOMEM;
        goto fail_pool;
    }
    struct nvm_qpair_mem am = {
        .qid = 0,
        .depth = (uint16_t)adepth,
        .sq = c->asq,
        .sq_iova = asq_iova,
        .cq = c->acq,
        .cq_iova = acq_iova,
        .sq_db = (volatile uint32_t *)((volatile char *)c->regs + nvme_sq_db_offset(0, c->dstrd)),
        .cq_db = (volatile uint32_t *)((volatile char *)c->regs + nvme_cq_db_offset(0, c->dstrd)),
    };
    if ((rc = nvm_qpair_init(&c->admin, &am, &c->pool, 2 * NVM_PAGE_SIZE, NVM_ADMIN_TIMEOUT_NS)) !=
        0)
        goto fail_pool;
    c->admin.ctrl = c;
    nvm_mmio_write32(c->regs, NVME_REG_AQA, NVME_AQA(adepth, adepth));
    nvm_mmio_write64(c->regs, NVME_REG_ASQ, asq_iova);
    nvm_mmio_write64(c->regs, NVME_REG_ACQ, acq_iova);

    if ((rc = ctrl_enable(c)) != 0)
        goto fail_admin;
    if ((rc = identify_controller(c)) != 0 || (rc = identify_namespaces(c)) != 0 ||
        (rc = negotiate_queues(c)) != 0) {
        if (rc > 0) {
            char sbuf[96];
            nvm_err("%s: admin command failed: %s", bdf,
                    nvm_status_str((uint32_t)rc, sbuf, sizeof(sbuf)));
            rc = -EIO;
        }
        goto fail_enabled;
    }
    nvm_info("%s: %s sn %s fw %s, %u namespace(s), %u I/O queue pairs, max transfer %u", bdf,
             c->info.model, c->info.serial, c->info.firmware, c->nns, c->info.max_qpairs,
             c->info.max_xfer);
    *out = c;
    return 0;

fail_enabled:
    ctrl_disable(c);
fail_admin:
    nvm_qpair_fini(&c->admin);
fail_pool:
    nvm_dma_pool_fini(&c->pool);
fail_vfio:
    nvm_vfio_close(&c->vfio);
fail_free:
    free(c->ns);
    free(c);
    return rc;
}

/* Normal shutdown (§7.6.2): CC.SHN = 01b, then wait for CSTS.SHST = 10b. */
static void ctrl_shutdown(struct nvm_ctrl *c)
{
    uint32_t cc = nvm_mmio_read32(c->regs, NVME_REG_CC);
    nvm_mmio_write32(c->regs, NVME_REG_CC, (cc & ~NVME_CC_SHN_MASK) | NVME_CC_SHN_NORMAL);
    /* The spec bounds this by RTD3E when reported; CAP.TO (at least 10 s
     * here) is a generous stand-in that does not need Identify. */
    uint64_t limit_ms = c->timeout_ms > 10000 ? c->timeout_ms : 10000;
    uint64_t deadline = nvm_now_ns() + limit_ms * 1000000ull;
    for (;;) {
        uint32_t s = csts(c);
        if (s == UINT32_MAX || (s & NVME_CSTS_SHST_MASK) == NVME_CSTS_SHST_DONE)
            break;
        if (nvm_now_ns() > deadline) {
            nvm_warn("%s: shutdown did not complete in %llu ms", c->info.bdf,
                     (unsigned long long)limit_ms);
            break;
        }
        usleep(1000);
    }
}

void nvm_ctrl_close(struct nvm_ctrl *c)
{
    if (!c)
        return;
    for (int qid = 1; qid <= NVM_MAX_QID; qid++) {
        if (c->qpairs[qid]) {
            nvm_warn("%s: queue pair %d still allocated at close; freeing it", c->info.bdf, qid);
            nvm_qpair_free(c->qpairs[qid]);
        }
    }
    ctrl_shutdown(c);
    if (c->nirq)
        nvm_irq_msix_disable(&c->vfio);
    for (uint32_t i = 0; i < c->nirq; i++)
        close(c->irq_fds[i]);
    nvm_qpair_fini(&c->admin);
    nvm_dma_pool_fini(&c->pool);
    nvm_vfio_close(&c->vfio);
    pthread_mutex_destroy(&c->admin_lock);
    pthread_mutex_destroy(&c->qid_lock);
    free(c->ns);
    free(c);
}

const struct nvm_ctrl_info *nvm_ctrl_info(const struct nvm_ctrl *c)
{
    return &c->info;
}

const struct nvm_ns_info *nvm_ctrl_ns(const struct nvm_ctrl *c, uint32_t nsid)
{
    for (uint32_t i = 0; i < c->nns; i++)
        if (c->ns[i].nsid == nsid)
            return &c->ns[i];
    return NULL;
}

const struct nvm_ns_info *nvm_ctrl_ns_at(const struct nvm_ctrl *c, uint32_t i)
{
    return i < c->nns ? &c->ns[i] : NULL;
}

/* ---- I/O queue pairs ---- */

/*
 * Enables MSI-X once, for every vector nvmetal can use, with one eventfd
 * each. All vectors at once because changing the set later means disabling
 * and re-enabling MSI-X on kernels without dynamic MSI-X allocation.
 * Called with qid_lock held.
 */
static int irq_setup(struct nvm_ctrl *c)
{
    if (c->nirq)
        return 0;
    uint32_t n = c->vfio.msix_count;
    if (n > (uint32_t)c->info.max_qpairs + 1)
        n = (uint32_t)c->info.max_qpairs + 1;
    if (n < 2) {
        nvm_err("%s: no MSI-X vectors for I/O queues", c->info.bdf);
        return -ENOTSUP;
    }
    for (uint32_t i = 0; i < n; i++) {
        c->irq_fds[i] = eventfd(0, EFD_CLOEXEC);
        if (c->irq_fds[i] < 0) {
            int e = -errno;
            while (i--)
                close(c->irq_fds[i]);
            return e;
        }
    }
    int rc = nvm_irq_msix_enable(&c->vfio, c->irq_fds, n);
    if (rc) {
        for (uint32_t i = 0; i < n; i++)
            close(c->irq_fds[i]);
        return rc;
    }
    c->nirq = n;
    return 0;
}

static int delete_queue(struct nvm_ctrl *c, uint8_t opc, uint16_t qid)
{
    struct nvme_sqe cmd = {.opc = opc, .cdw10 = qid};
    return nvm_admin_sync(c, &cmd, NULL, 0, NULL);
}

int nvm_qpair_alloc(struct nvm_ctrl *c, const struct nvm_qpair_opts *opts, struct nvm_qpair **out)
{
    uint32_t depth = opts && opts->depth ? opts->depth : NVM_IO_DEPTH_DEFAULT;
    /* Ring indices are 16-bit; MQES allows up to 65536 entries, nvmetal 32768. */
    if (depth < 2 || depth > c->info.max_queue_entries || depth > 32768)
        return -EINVAL;

    int irq = opts && opts->mode == NVM_QP_IRQ;
    pthread_mutex_lock(&c->qid_lock);
    if (irq) {
        int rc = irq_setup(c);
        if (rc) {
            pthread_mutex_unlock(&c->qid_lock);
            return rc;
        }
    }
    uint16_t qid = 0;
    for (uint16_t q = 1; q <= c->info.max_qpairs; q++) {
        if (irq && q >= c->nirq)
            break;
        if (!c->qpairs[q]) {
            qid = q;
            /* Reserve the slot; the real pointer is stored on success. */
            c->qpairs[q] = (struct nvm_qpair *)(uintptr_t)1;
            break;
        }
    }
    pthread_mutex_unlock(&c->qid_lock);
    if (!qid)
        return -EBUSY;

    int rc = -ENOMEM;
    struct nvm_qpair *qp = NULL;
    uint64_t sq_iova = 0, cq_iova = 0;
    void *sq = nvm_dma_pool_alloc(&c->pool, depth * sizeof(struct nvme_sqe), &sq_iova);
    void *cq = nvm_dma_pool_alloc(&c->pool, depth * sizeof(struct nvme_cqe), &cq_iova);
    if (posix_memalign((void **)&qp, 64, sizeof(*qp)) != 0)
        qp = NULL;
    if (!sq || !cq || !qp)
        goto fail;

    struct nvm_qpair_mem m = {
        .qid = qid,
        .depth = (uint16_t)depth,
        .sq = sq,
        .sq_iova = sq_iova,
        .cq = cq,
        .cq_iova = cq_iova,
        .sq_db = (volatile uint32_t *)((volatile char *)c->regs + nvme_sq_db_offset(qid, c->dstrd)),
        .cq_db = (volatile uint32_t *)((volatile char *)c->regs + nvme_cq_db_offset(qid, c->dstrd)),
    };
    uint64_t tmo_ns = opts && opts->timeout_ms ? opts->timeout_ms * 1000000ull : NVM_IO_TIMEOUT_NS;
    if ((rc = nvm_qpair_init(qp, &m, &c->pool, c->info.max_xfer, tmo_ns)) != 0)
        goto fail;
    qp->ctrl = c;

    /* Create I/O CQ (§5.3): physically contiguous; interrupts off when polled,
     * else IEN with vector IV = QID. */
    struct nvme_sqe cmd = {
        .opc = NVME_ADMIN_CREATE_CQ,
        .prp1 = cq_iova,
        .cdw10 = (depth - 1) << 16 | qid,
        .cdw11 = irq ? (uint32_t)qid << 16 | NVME_CQ_IEN | NVME_CQ_PC : NVME_CQ_PC,
    };
    if (irq) {
        qp->irq_fd = c->irq_fds[qid];
        nvm_irq_drain(qp->irq_fd); /* left over from a previous user of the vector */
    }
    if ((rc = nvm_admin_sync(c, &cmd, NULL, 0, NULL)) != 0)
        goto fail_init;
    /* Create I/O SQ (§5.4) bound to the CQ with the same QID. */
    cmd = (struct nvme_sqe){
        .opc = NVME_ADMIN_CREATE_SQ,
        .prp1 = sq_iova,
        .cdw10 = (depth - 1) << 16 | qid,
        .cdw11 = (uint32_t)qid << 16 | NVME_SQ_PC,
    };
    if ((rc = nvm_admin_sync(c, &cmd, NULL, 0, NULL)) != 0) {
        delete_queue(c, NVME_ADMIN_DELETE_CQ, qid);
        goto fail_init;
    }

    pthread_mutex_lock(&c->qid_lock);
    c->qpairs[qid] = qp;
    pthread_mutex_unlock(&c->qid_lock);
    *out = qp;
    return 0;

fail_init:
    nvm_qpair_fini(qp);
    if (rc > 0) {
        char sbuf[96];
        nvm_err("creating queue pair %u: %s", qid,
                nvm_status_str((uint32_t)rc, sbuf, sizeof(sbuf)));
        rc = -EIO;
    }
fail:
    nvm_dma_pool_free(&c->pool, sq);
    nvm_dma_pool_free(&c->pool, cq);
    free(qp);
    pthread_mutex_lock(&c->qid_lock);
    c->qpairs[qid] = NULL;
    pthread_mutex_unlock(&c->qid_lock);
    return rc;
}

void nvm_qpair_free(struct nvm_qpair *qp)
{
    if (!qp)
        return;
    struct nvm_ctrl *c = qp->ctrl;
    /* §5.6/§5.7: the SQ must be deleted before the CQ it posts to. Deleting
     * the SQ aborts whatever the controller has not finished. */
    int rc = delete_queue(c, NVME_ADMIN_DELETE_SQ, qp->qid);
    if (rc == 0)
        rc = delete_queue(c, NVME_ADMIN_DELETE_CQ, qp->qid);
    if (rc)
        nvm_warn("deleting queue pair %u failed (%d)", qp->qid, rc);
    uint16_t qid = qp->qid;
    nvm_qpair_fini(qp);
    nvm_dma_pool_free(&c->pool, qp->sq);
    nvm_dma_pool_free(&c->pool, qp->cq);
    free(qp);
    pthread_mutex_lock(&c->qid_lock);
    c->qpairs[qid] = NULL;
    pthread_mutex_unlock(&c->qid_lock);
}

void *nvm_dma_alloc(struct nvm_ctrl *c, size_t size, uint64_t *iova)
{
    return nvm_dma_pool_alloc(&c->pool, size, iova);
}

void nvm_dma_free(struct nvm_ctrl *c, void *ptr)
{
    nvm_dma_pool_free(&c->pool, ptr);
}
