/*
 * nvmetal-identify: print what a controller reports about itself and its
 * namespaces, and check that I/O queue pairs can be created and deleted.
 * Usage: nvmetal-identify [--json] <bdf>
 */
#include "nvmetal/nvmetal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int probe_queues(struct nvm_ctrl *c, int *created)
{
    /* Allocate shallow queue pairs until the controller runs out, then free
     * them: exercises Create/Delete I/O CQ+SQ for every granted QID. */
    struct nvm_qpair *qps[64];
    int n = 0, rc = 0;
    while (n < 64) {
        struct nvm_qpair_opts o = {.depth = 2};
        rc = nvm_qpair_alloc(c, &o, &qps[n]);
        if (rc)
            break;
        n++;
    }
    *created = n;
    for (int i = 0; i < n; i++)
        nvm_qpair_free(qps[i]);
    return rc == -EBUSY ? 0 : rc;
}

int main(int argc, char **argv)
{
    int json = 0;
    const char *bdf = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json"))
            json = 1;
        else
            bdf = argv[i];
    }
    if (!bdf) {
        fprintf(stderr, "usage: %s [--json] <pci-bdf>\n", argv[0]);
        return 2;
    }
    struct nvm_ctrl *c;
    int rc = nvm_ctrl_open(bdf, NULL, &c);
    if (rc) {
        fprintf(stderr, "open %s: %s\n", bdf, strerror(-rc));
        return 1;
    }
    const struct nvm_ctrl_info *in = nvm_ctrl_info(c);
    int created = 0;
    int qrc = probe_queues(c, &created);
    uint64_t cap = in->cap;

    if (json) {
        printf("{\"bdf\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\",\"firmware\":\"%s\","
               "\"vid\":%u,\"version\":\"%u.%u.%u\",\"mdts_bytes\":%u,\"max_xfer\":%u,"
               "\"max_queue_entries\":%u,\"max_qpairs\":%u,\"qpairs_created\":%d,"
               "\"cap_to_ms\":%u,\"dstrd\":%u,\"oacs\":%u,\"oncs\":%u,\"vwc\":%u,\"nn\":%u,"
               "\"iommu\":%d,\"msix\":%u,\"namespaces\":[",
               in->bdf, in->model, in->serial, in->firmware, in->vid, in->version >> 16,
               (in->version >> 8) & 0xff, in->version & 0xff, in->mdts_bytes, in->max_xfer,
               in->max_queue_entries, in->max_qpairs, created, (unsigned)((cap >> 24) & 0xff) * 500,
               (unsigned)((cap >> 32) & 0xf), in->oacs, in->oncs, in->vwc, in->nn, in->iommu,
               in->msix_vectors);
        for (uint32_t i = 0; i < in->num_ns; i++) {
            const struct nvm_ns_info *ns = nvm_ctrl_ns_at(c, i);
            printf("%s{\"nsid\":%u,\"nlba\":%" PRIu64 ",\"lba_size\":%u,\"ms\":%u,"
                   "\"bytes\":%" PRIu64 "}",
                   i ? "," : "", ns->nsid, ns->nlba, ns->lba_size, ns->ms, ns->nlba * ns->lba_size);
        }
        printf("]}\n");
    } else {
        printf("controller   %s\n", in->bdf);
        printf("model        %s\n", in->model);
        printf("serial       %s\n", in->serial);
        printf("firmware     %s\n", in->firmware);
        printf("vendor       0x%04x (subsystem 0x%04x)\n", in->vid, in->ssvid);
        printf("version      %u.%u.%u\n", in->version >> 16, (in->version >> 8) & 0xff,
               in->version & 0xff);
        if (in->mdts_bytes)
            printf("MDTS         %u KiB\n", in->mdts_bytes / 1024);
        else
            printf("MDTS         unlimited\n");
        printf("max transfer %u KiB (nvmetal)\n", in->max_xfer / 1024);
        printf("queues       %u I/O queue pairs granted, %u entries max (CAP.MQES+1); "
               "created/deleted %d\n",
               in->max_qpairs, in->max_queue_entries, created);
        printf("CAP          0x%016" PRIx64 " (TO %u ms, DSTRD %u, CQR %u)\n", cap,
               (unsigned)((cap >> 24) & 0xff) * 500, (unsigned)((cap >> 32) & 0xf),
               (unsigned)((cap >> 16) & 1));
        printf("features     ONCS 0x%04x (write zeroes %s, DSM %s), VWC %s, OACS 0x%04x\n",
               in->oncs, (in->oncs & 8) ? "yes" : "no", (in->oncs & 4) ? "yes" : "no",
               (in->vwc & 1) ? "present" : "absent", in->oacs);
        printf("DMA          %s, %u MSI-X vectors\n",
               in->iommu ? "IOMMU (VFIO type1)" : "physical addresses (no-IOMMU)",
               in->msix_vectors);
        printf("namespaces   %u active (NN %u)\n", in->num_ns, in->nn);
        for (uint32_t i = 0; i < in->num_ns; i++) {
            const struct nvm_ns_info *ns = nvm_ctrl_ns_at(c, i);
            printf("  nsid %u: %" PRIu64 " LBAs x %u B = %.2f GiB, metadata %u B, "
                   "format %u of %u\n",
                   ns->nsid, ns->nlba, ns->lba_size, (double)(ns->nlba * ns->lba_size) / (1 << 30),
                   ns->ms, ns->flbas & 0xf, ns->nlbaf + 1);
        }
    }
    nvm_ctrl_close(c);
    if (qrc) {
        fprintf(stderr, "queue pair probe failed: %s\n", strerror(-qrc));
        return 1;
    }
    return 0;
}
