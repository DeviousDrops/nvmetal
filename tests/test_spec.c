/* Spec layout checks beyond the static asserts: field packing and decode macros. */
#include "nvme_spec.h"
#include "test.h"

TEST(sqe_cdw0_packing)
{
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc = NVME_CMD_READ;
    sqe.cid = 0xbeef;
    uint32_t cdw0;
    memcpy(&cdw0, &sqe, 4);
    /* §4.2: opcode in CDW0 7:0, CID in 31:16 (little endian host). */
    CHECK_EQ(cdw0 & 0xff, NVME_CMD_READ);
    CHECK_EQ(cdw0 >> 16, 0xbeef);
}

TEST(cqe_dw3_decode)
{
    /* CID 0x1234, phase 1, SCT 2 (media), SC 0x81, DNR set. */
    uint32_t sf = (1u << 14) | (2u << 8) | 0x81;
    uint32_t dw3 = 0x1234 | (1u << 16) | (sf << 17);
    CHECK_EQ(NVME_CQE_CID(dw3), 0x1234);
    CHECK_EQ(NVME_CQE_PHASE(dw3), 1);
    uint16_t st = NVME_CQE_STATUS(dw3);
    CHECK_EQ(NVME_SF_SC(st), 0x81);
    CHECK_EQ(NVME_SF_SCT(st), NVME_SCT_MEDIA);
    CHECK_EQ(NVME_SF_DNR(st), 1);
    CHECK_EQ(NVME_SF_MORE(st), 0);
}

TEST(cap_decode)
{
    /* MQES=2047, CQR=1, TO=0x0f, DSTRD=0, CSS=NVM, MPSMIN=0, MPSMAX=4 */
    uint64_t cap = 2047ull | 1ull << 16 | 0x0full << 24 | 1ull << 37 | 4ull << 52;
    CHECK_EQ(NVME_CAP_MQES(cap), 2047);
    CHECK_EQ(NVME_CAP_CQR(cap), 1);
    CHECK_EQ(NVME_CAP_TO(cap), 0x0f);
    CHECK_EQ(NVME_CAP_DSTRD(cap), 0);
    CHECK_EQ(NVME_CAP_CSS(cap) & NVME_CAP_CSS_NVM, 1);
    CHECK_EQ(NVME_CAP_MPSMIN(cap), 0);
    CHECK_EQ(NVME_CAP_MPSMAX(cap), 4);
}

TEST(doorbell_offsets)
{
    CHECK_EQ(nvme_sq_db_offset(0, 0), 0x1000);
    CHECK_EQ(nvme_cq_db_offset(0, 0), 0x1004);
    CHECK_EQ(nvme_sq_db_offset(3, 0), 0x1018);
    CHECK_EQ(nvme_cq_db_offset(3, 0), 0x101c);
    /* DSTRD=2 means a 16-byte stride. */
    CHECK_EQ(nvme_sq_db_offset(1, 2), 0x1020);
    CHECK_EQ(nvme_cq_db_offset(1, 2), 0x1030);
}

TEST(aqa_encoding)
{
    CHECK_EQ(NVME_AQA(32, 32), (31u << 16) | 31u);
    CHECK_EQ(NVME_AQA(2, 4096), (4095u << 16) | 1u);
}

TEST_MAIN()
