#ifndef NVMETAL_CTRL_H
#define NVMETAL_CTRL_H

#include "dma.h"
#include "nvmetal/nvmetal.h"
#include "qpair.h"
#include "vfio.h"

#include <pthread.h>

#define NVM_MAX_QID 64

struct nvm_ctrl {
    struct nvm_vfio vfio;
    struct nvm_iova_vfio_ctx iova_ctx;
    struct nvm_dma_pool pool;
    volatile void *regs; /* BAR0 */
    uint64_t cap;
    uint32_t dstrd;
    uint32_t timeout_ms; /* CAP.TO in milliseconds */

    pthread_mutex_t admin_lock; /* serialises admin commands and admin_buf */
    struct nvm_qpair admin;
    void *asq;
    void *acq;
    void *admin_buf; /* 4 KiB scratch for Identify and friends */
    uint64_t admin_buf_iova;

    struct nvm_ctrl_info info;
    struct nvm_ns_info *ns;
    uint32_t nns;

    pthread_mutex_t qid_lock;
    struct nvm_qpair *qpairs[NVM_MAX_QID + 1]; /* by QID, NULL when free */

    /* MSI-X, enabled on the first interrupt-mode queue pair (under qid_lock). */
    int irq_fds[NVM_MAX_QID + 1]; /* eventfd per vector; vector q serves QID q */
    uint32_t nirq;
};

/*
 * Submits one admin command and spins until it completes. buf/len, if set,
 * must be DMA memory; PRP entries are filled in for transfers of up to two
 * pages. Returns 0, a positive NVMe status, or -errno (-ETIMEDOUT).
 */
int nvm_admin_sync(struct nvm_ctrl *ctrl, struct nvme_sqe *cmd, void *buf, size_t len,
                   struct nvm_cpl *out);

#endif
