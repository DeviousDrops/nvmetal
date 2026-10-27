/*
 * NVMe register, queue-entry and command definitions used by nvmetal.
 *
 * Section numbers refer to the NVM Express Base Specification, revision 1.4c
 * (the last revision that keeps the NVM command set in the base document, so
 * one document covers everything here). Section titles are given as well, so
 * the definitions can be found in 2.x revisions where numbering moved.
 */
#ifndef NVMETAL_NVME_SPEC_H
#define NVMETAL_NVME_SPEC_H

#include <stddef.h>
#include <stdint.h>

/* ---- Controller registers in BAR0 (§3.1 "Register Definition") ---- */

#define NVME_REG_CAP   0x00   /* §3.1.1 Controller Capabilities (64-bit) */
#define NVME_REG_VS    0x08   /* §3.1.2 Version */
#define NVME_REG_INTMS 0x0c   /* §3.1.3 Interrupt Mask Set */
#define NVME_REG_INTMC 0x10   /* §3.1.4 Interrupt Mask Clear */
#define NVME_REG_CC    0x14   /* §3.1.5 Controller Configuration */
#define NVME_REG_CSTS  0x1c   /* §3.1.6 Controller Status */
#define NVME_REG_NSSR  0x20   /* §3.1.7 NVM Subsystem Reset */
#define NVME_REG_AQA   0x24   /* §3.1.8 Admin Queue Attributes */
#define NVME_REG_ASQ   0x28   /* §3.1.9 Admin Submission Queue Base Address (64-bit) */
#define NVME_REG_ACQ   0x30   /* §3.1.10 Admin Completion Queue Base Address (64-bit) */
#define NVME_REG_DBS   0x1000 /* §3.1 SQ0TDBL; doorbell stride is CAP.DSTRD */

/* CAP fields (§3.1.1). */
#define NVME_CAP_MQES(cap)   ((uint32_t)((cap) & 0xffff))       /* 0-based max queue entries */
#define NVME_CAP_CQR(cap)    ((uint32_t)(((cap) >> 16) & 0x1))  /* contiguous queues required */
#define NVME_CAP_TO(cap)     ((uint32_t)(((cap) >> 24) & 0xff)) /* timeout, 500 ms units */
#define NVME_CAP_DSTRD(cap)  ((uint32_t)(((cap) >> 32) & 0xf))  /* doorbell stride 2^(2+n) */
#define NVME_CAP_NSSRS(cap)  ((uint32_t)(((cap) >> 36) & 0x1))
#define NVME_CAP_CSS(cap)    ((uint32_t)(((cap) >> 37) & 0xff))
#define NVME_CAP_MPSMIN(cap) ((uint32_t)(((cap) >> 48) & 0xf)) /* page size 2^(12+n) */
#define NVME_CAP_MPSMAX(cap) ((uint32_t)(((cap) >> 52) & 0xf))
#define NVME_CAP_CSS_NVM     0x1

/* CC fields (§3.1.5). */
#define NVME_CC_EN         (1u << 0)
#define NVME_CC_CSS_NVM    (0u << 4)
#define NVME_CC_MPS(n)     ((uint32_t)(n) << 7)
#define NVME_CC_AMS_RR     (0u << 11)
#define NVME_CC_SHN_MASK   (3u << 14)
#define NVME_CC_SHN_NORMAL (1u << 14)
#define NVME_CC_SHN_ABRUPT (2u << 14)
#define NVME_CC_IOSQES(n)  ((uint32_t)(n) << 16)
#define NVME_CC_IOCQES(n)  ((uint32_t)(n) << 20)

/* CSTS fields (§3.1.6). */
#define NVME_CSTS_RDY         (1u << 0)
#define NVME_CSTS_CFS         (1u << 1) /* controller fatal status */
#define NVME_CSTS_SHST_MASK   (3u << 2)
#define NVME_CSTS_SHST_NORMAL (0u << 2)
#define NVME_CSTS_SHST_OCCUR  (1u << 2)
#define NVME_CSTS_SHST_DONE   (2u << 2)

/* AQA fields (§3.1.8), both 0-based sizes. */
#define NVME_AQA(sq, cq) ((uint32_t)(((cq) - 1) << 16 | ((sq) - 1)))

/*
 * Doorbell offsets (§3.1 "SQyTDBL"/"CQyHDBL"): queue y's SQ tail doorbell is at
 * 1000h + (2y * (4 << DSTRD)), its CQ head doorbell at 1000h + ((2y + 1) * (4 << DSTRD)).
 */
static inline size_t nvme_sq_db_offset(uint16_t qid, uint32_t dstrd)
{
    return NVME_REG_DBS + (size_t)(2u * qid) * (4u << dstrd);
}

static inline size_t nvme_cq_db_offset(uint16_t qid, uint32_t dstrd)
{
    return NVME_REG_DBS + (size_t)(2u * qid + 1u) * (4u << dstrd);
}

/* ---- Queue entries ---- */

/* §4.2 "Submission Queue Entry - Command Format": 64 bytes. */
struct nvme_sqe {
    uint8_t opc;   /* CDW0 bits 7:0 opcode */
    uint8_t flags; /* CDW0 bits 9:8 FUSE, bits 15:14 PSDT (00b = PRPs) */
    uint16_t cid;  /* CDW0 bits 31:16 command identifier */
    uint32_t nsid; /* CDW1 */
    uint32_t cdw2; /* CDW2-3 reserved for NVM/admin commands used here */
    uint32_t cdw3;
    uint64_t mptr; /* CDW4-5 metadata pointer */
    uint64_t prp1; /* CDW6-7 data pointer, PRP entry 1 */
    uint64_t prp2; /* CDW8-9 PRP entry 2 or PRP list pointer */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

/*
 * §4.6 "Completion Queue Entry": 16 bytes. DW3 holds the CID in bits 15:0, the
 * phase tag in bit 16 and the status field in bits 31:17. Keeping DW3 as a
 * single 32-bit word lets the poller load CID, phase and status in one access.
 */
struct nvme_cqe {
    uint32_t dw0;  /* command specific */
    uint32_t dw1;  /* reserved */
    uint16_t sqhd; /* DW2 bits 15:0 SQ head pointer */
    uint16_t sqid; /* DW2 bits 31:16 SQ identifier */
    uint32_t dw3;  /* CID | P << 16 | SF << 17 */
};

#define NVME_CQE_CID(dw3)    ((uint16_t)((dw3) & 0xffff))
#define NVME_CQE_PHASE(dw3)  (((dw3) >> 16) & 0x1)
#define NVME_CQE_STATUS(dw3) ((uint16_t)((dw3) >> 17)) /* SF without the phase bit */

/* Status field decode (§4.6.1 "Status Field Definition"), on the 15-bit SF. */
#define NVME_SF_SC(sf)   ((sf) & 0xff)
#define NVME_SF_SCT(sf)  (((sf) >> 8) & 0x7)
#define NVME_SF_CRD(sf)  (((sf) >> 11) & 0x3)
#define NVME_SF_MORE(sf) (((sf) >> 13) & 0x1)
#define NVME_SF_DNR(sf)  (((sf) >> 14) & 0x1)

/* Status code types (§4.6.1.1). */
#define NVME_SCT_GENERIC  0x0
#define NVME_SCT_CMD_SPEC 0x1
#define NVME_SCT_MEDIA    0x2
#define NVME_SCT_PATH     0x3
#define NVME_SCT_VENDOR   0x7

/* Generic command status (§4.6.1.2.1). */
#define NVME_SC_SUCCESS            0x00
#define NVME_SC_INVALID_OPCODE     0x01
#define NVME_SC_INVALID_FIELD      0x02
#define NVME_SC_CID_CONFLICT       0x03
#define NVME_SC_DATA_XFER_ERROR    0x04
#define NVME_SC_POWER_LOSS         0x05
#define NVME_SC_INTERNAL           0x06
#define NVME_SC_ABORT_REQ          0x07
#define NVME_SC_ABORT_SQ_DELETED   0x08
#define NVME_SC_INVALID_NS         0x0b
#define NVME_SC_PRP_OFFSET_INVALID 0x13
#define NVME_SC_LBA_RANGE          0x80
#define NVME_SC_CAP_EXCEEDED       0x81
#define NVME_SC_NS_NOT_READY       0x82

/* Command specific status (§4.6.1.2.2), the ones queue creation can return. */
#define NVME_SC_CQ_INVALID       0x00
#define NVME_SC_QID_INVALID      0x01
#define NVME_SC_QUEUE_SIZE       0x02
#define NVME_SC_INVALID_LOG_PAGE 0x09
#define NVME_SC_INVALID_FORMAT   0x0a
#define NVME_SC_INVALID_QDELETE  0x0c

/* Media and data integrity errors (§4.6.1.2.3). */
#define NVME_SC_WRITE_FAULT   0x80
#define NVME_SC_READ_ERROR    0x81
#define NVME_SC_GUARD_CHECK   0x82
#define NVME_SC_APPTAG_CHECK  0x83
#define NVME_SC_REFTAG_CHECK  0x84
#define NVME_SC_ACCESS_DENIED 0x86

/* ---- Opcodes ---- */

/* Admin command set (§5, Figure 139 "Opcodes for Admin Commands"). */
#define NVME_ADMIN_DELETE_SQ    0x00 /* §5.7 */
#define NVME_ADMIN_CREATE_SQ    0x01 /* §5.4 */
#define NVME_ADMIN_GET_LOG      0x02 /* §5.14 */
#define NVME_ADMIN_DELETE_CQ    0x04 /* §5.6 */
#define NVME_ADMIN_CREATE_CQ    0x05 /* §5.3 */
#define NVME_ADMIN_IDENTIFY     0x06 /* §5.15 */
#define NVME_ADMIN_SET_FEATURES 0x09 /* §5.21 */
#define NVME_ADMIN_GET_FEATURES 0x0a /* §5.9 */

/* NVM command set (§6). */
#define NVME_CMD_FLUSH        0x00 /* §6.8 */
#define NVME_CMD_WRITE        0x01 /* §6.15 */
#define NVME_CMD_READ         0x02 /* §6.9 */
#define NVME_CMD_WRITE_ZEROES 0x08 /* §6.16 */
#define NVME_CMD_DSM          0x09 /* §6.7 */

/* Identify CNS values (§5.15, Figure 245). */
#define NVME_CNS_NS          0x00
#define NVME_CNS_CTRL        0x01
#define NVME_CNS_ACTIVE_NSID 0x02

/* Feature identifiers (§5.21.1). */
#define NVME_FEAT_NUM_QUEUES 0x07 /* §5.21.1.7 */

/* Log page identifiers (§5.14.1). */
#define NVME_LOG_SMART 0x02 /* §5.14.1.2 */

/* Create I/O CQ CDW11 (§5.3): PC bit 0, IEN bit 1, IV bits 31:16. */
#define NVME_CQ_PC  (1u << 0)
#define NVME_CQ_IEN (1u << 1)
/* Create I/O SQ CDW11 (§5.4): PC bit 0, QPRIO bits 2:1, CQID bits 31:16. */
#define NVME_SQ_PC (1u << 0)

/* DSM attributes (§6.7): deallocate bit in CDW11. */
#define NVME_DSM_AD (1u << 2)

/* ONCS bits in Identify Controller (§5.15.2, Figure 247). */
#define NVME_ONCS_DSM          (1u << 2)
#define NVME_ONCS_WRITE_ZEROES (1u << 3)

/* ---- Identify data structures (§5.15.2) ---- */

/* Identify Controller (CNS 01h, Figure 247): only fields nvmetal uses are named. */
struct nvme_id_ctrl {
    uint16_t vid;
    uint16_t ssvid;
    char sn[20];
    char mn[40];
    char fr[8];
    uint8_t rab;
    uint8_t ieee[3];
    uint8_t cmic;
    uint8_t mdts; /* max transfer = 2^mdts units of CAP.MPSMIN; 0 = unlimited */
    uint16_t cntlid;
    uint32_t ver;
    uint8_t rsvd84[172];
    uint16_t oacs; /* offset 256 */
    uint8_t acl;
    uint8_t aerl;
    uint8_t frmw;
    uint8_t lpa;
    uint8_t elpe;
    uint8_t npss;
    uint8_t rsvd264[248];
    uint8_t sqes; /* offset 512: bits 3:0 required, 7:4 maximum (log2) */
    uint8_t cqes;
    uint16_t maxcmd;
    uint32_t nn; /* number of namespaces */
    uint16_t oncs;
    uint16_t fuses;
    uint8_t fna;
    uint8_t vwc; /* bit 0: volatile write cache present */
    uint16_t awun;
    uint16_t awupf;
    uint8_t rsvd532[3564];
};

/* LBA Format data structure (§5.15.2, Figure 250). */
struct nvme_lbaf {
    uint16_t ms;   /* metadata size */
    uint8_t lbads; /* LBA data size, 2^n bytes */
    uint8_t rp;    /* relative performance, bits 1:0 */
};

/* Identify Namespace (CNS 00h, Figure 249). */
struct nvme_id_ns {
    uint64_t nsze;
    uint64_t ncap;
    uint64_t nuse;
    uint8_t nsfeat;
    uint8_t nlbaf; /* 0-based number of LBA formats */
    uint8_t flbas; /* bits 3:0 index of the format in use */
    uint8_t mc;
    uint8_t dpc;
    uint8_t dps;
    uint8_t nmic;
    uint8_t rescap;
    uint8_t rsvd32[96];
    struct nvme_lbaf lbaf[16]; /* offset 128 */
    uint8_t rsvd192[3904];
};

/* SMART / Health Information log page (§5.14.1.2, Figure 194). */
struct nvme_smart_log {
    uint8_t critical_warning;
    uint8_t temperature[2]; /* Kelvin, little endian, unaligned in the spec */
    uint8_t avail_spare;
    uint8_t spare_thresh;
    uint8_t percent_used;
    uint8_t endurance_grp;
    uint8_t rsvd7[25];
    uint8_t data_units_read[16];
    uint8_t data_units_written[16];
    uint8_t host_reads[16];
    uint8_t host_writes[16];
    uint8_t ctrl_busy_time[16];
    uint8_t power_cycles[16];
    uint8_t power_on_hours[16];
    uint8_t unsafe_shutdowns[16];
    uint8_t media_errors[16];
    uint8_t num_err_log_entries[16];
    uint8_t rsvd192[320];
};

/* Dataset Management range (§6.7, Figure 364). */
struct nvme_dsm_range {
    uint32_t cattr;
    uint32_t nlb;
    uint64_t slba;
};

/* ---- Layout checks: the device reads these bytes, so every offset matters ---- */

_Static_assert(sizeof(struct nvme_sqe) == 64, "SQE is 64 bytes (§4.2)");
_Static_assert(offsetof(struct nvme_sqe, cid) == 2, "CID in CDW0 31:16");
_Static_assert(offsetof(struct nvme_sqe, nsid) == 4, "NSID is CDW1");
_Static_assert(offsetof(struct nvme_sqe, mptr) == 16, "MPTR is CDW4-5");
_Static_assert(offsetof(struct nvme_sqe, prp1) == 24, "PRP1 is CDW6-7");
_Static_assert(offsetof(struct nvme_sqe, prp2) == 32, "PRP2 is CDW8-9");
_Static_assert(offsetof(struct nvme_sqe, cdw10) == 40, "CDW10 at byte 40");
_Static_assert(offsetof(struct nvme_sqe, cdw15) == 60, "CDW15 at byte 60");

_Static_assert(sizeof(struct nvme_cqe) == 16, "CQE is 16 bytes (§4.6)");
_Static_assert(offsetof(struct nvme_cqe, sqhd) == 8, "SQHD is DW2 15:0");
_Static_assert(offsetof(struct nvme_cqe, sqid) == 10, "SQID is DW2 31:16");
_Static_assert(offsetof(struct nvme_cqe, dw3) == 12, "CID/P/SF are DW3");

_Static_assert(sizeof(struct nvme_id_ctrl) == 4096, "Identify Controller is 4 KiB");
_Static_assert(offsetof(struct nvme_id_ctrl, sn) == 4, "SN at 4");
_Static_assert(offsetof(struct nvme_id_ctrl, mn) == 24, "MN at 24");
_Static_assert(offsetof(struct nvme_id_ctrl, fr) == 64, "FR at 64");
_Static_assert(offsetof(struct nvme_id_ctrl, mdts) == 77, "MDTS at 77");
_Static_assert(offsetof(struct nvme_id_ctrl, ver) == 80, "VER at 80");
_Static_assert(offsetof(struct nvme_id_ctrl, oacs) == 256, "OACS at 256");
_Static_assert(offsetof(struct nvme_id_ctrl, sqes) == 512, "SQES at 512");
_Static_assert(offsetof(struct nvme_id_ctrl, maxcmd) == 514, "MAXCMD at 514");
_Static_assert(offsetof(struct nvme_id_ctrl, nn) == 516, "NN at 516");
_Static_assert(offsetof(struct nvme_id_ctrl, oncs) == 520, "ONCS at 520");
_Static_assert(offsetof(struct nvme_id_ctrl, vwc) == 525, "VWC at 525");

_Static_assert(sizeof(struct nvme_lbaf) == 4, "LBA format is 4 bytes");
_Static_assert(sizeof(struct nvme_id_ns) == 4096, "Identify Namespace is 4 KiB");
_Static_assert(offsetof(struct nvme_id_ns, nlbaf) == 25, "NLBAF at 25");
_Static_assert(offsetof(struct nvme_id_ns, flbas) == 26, "FLBAS at 26");
_Static_assert(offsetof(struct nvme_id_ns, lbaf) == 128, "LBAF0 at 128");

_Static_assert(sizeof(struct nvme_smart_log) == 512, "SMART log is 512 bytes");
_Static_assert(offsetof(struct nvme_smart_log, data_units_read) == 32, "DUR at 32");
_Static_assert(offsetof(struct nvme_smart_log, media_errors) == 160, "media errors at 160");

_Static_assert(sizeof(struct nvme_dsm_range) == 16, "DSM range is 16 bytes");

#endif /* NVMETAL_NVME_SPEC_H */
