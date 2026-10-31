/*
 * MSI-X interrupt mode against a real controller: a wait with nothing
 * outstanding times out; 20000 reads at QD 16 complete using only
 * interrupt-driven waits; synchronous I/O works in interrupt mode; a polled
 * and an interrupt-driven queue pair coexist on one controller.
 * Usage: irq_smoke <bdf>
 */
#include "nvmetal/nvmetal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;
#define EXPECT(cond, ...)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int inflight, completed, errors;
static void cb(void *arg, const struct nvm_cpl *cpl)
{
    (void)arg;
    inflight--;
    completed++;
    errors += nvm_status_is_error(cpl->status);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <bdf>\n", argv[0]);
        return 2;
    }
    struct nvm_ctrl *c;
    int rc = nvm_ctrl_open(argv[1], NULL, &c);
    if (rc) {
        fprintf(stderr, "open: %s\n", strerror(-rc));
        return 1;
    }
    const struct nvm_ns_info *ns = nvm_ctrl_ns_at(c, 0);
    struct nvm_qpair *iq, *pq;
    struct nvm_qpair_opts io = {.depth = 32, .mode = NVM_QP_IRQ};
    struct nvm_qpair_opts po = {.depth = 32, .mode = NVM_QP_POLL};
    rc = nvm_qpair_alloc(c, &io, &iq);
    EXPECT(rc == 0, "irq qpair: %d", rc);
    rc = nvm_qpair_alloc(c, &po, &pq);
    EXPECT(rc == 0, "poll qpair: %d", rc);
    if (failures)
        return 1;
    EXPECT(nvm_qpair_irq_fd(iq) >= 0, "irq qpair has no eventfd");
    EXPECT(nvm_qpair_irq_fd(pq) < 0, "poll qpair has an eventfd");
    printf("  irq qpair qid %u, poll qpair qid %u\n", nvm_qpair_id(iq), nvm_qpair_id(pq));

    /* Nothing outstanding: the wait must time out, not hang or spin out. */
    double t0 = now_ms();
    rc = nvm_qpair_wait(iq, 8, 50);
    double waited = now_ms() - t0;
    EXPECT(rc == 0 && waited >= 45, "idle wait returned %d after %.1f ms", rc, waited);
    printf("  idle wait: returned %d after %.1f ms\n", rc, waited);

    enum { QD = 16, TOTAL = 20000 };
    uint8_t *buf = nvm_dma_alloc(c, QD * 4096, NULL);
    uint32_t nlb = 4096 / ns->lba_size;
    int submitted = 0;
    t0 = now_ms();
    while (completed < TOTAL) {
        while (inflight < QD && submitted < TOTAL) {
            uint64_t lba = (uint64_t)(submitted * 7919 % 100000) * nlb;
            rc = nvm_read(iq, ns->nsid, buf + (submitted % QD) * 4096, lba, nlb, cb, NULL);
            if (rc)
                break;
            inflight++;
            submitted++;
        }
        rc = nvm_qpair_wait(iq, QD, 2000);
        if (rc <= 0) {
            EXPECT(0, "wait returned %d with %d in flight", rc, inflight);
            break;
        }
    }
    double ms = now_ms() - t0;
    struct nvm_qpair_stats st;
    nvm_qpair_get_stats(iq, &st);
    EXPECT(completed == TOTAL && errors == 0, "completed %d errors %d", completed, errors);
    EXPECT(st.irq_wakeups > 0, "no interrupt wakeups");
    printf("  %d reads at QD %d in %.0f ms: %llu interrupt wakeups (%llu spurious), "
           "%.2f completions per wakeup\n",
           completed, QD, ms, (unsigned long long)st.irq_wakeups,
           (unsigned long long)st.spurious_wakeups,
           st.irq_wakeups ? (double)completed / (double)st.irq_wakeups : 0.0);

    /* Synchronous helpers sleep on the interrupt too. */
    uint8_t *w = nvm_dma_alloc(c, 65536, NULL), *r = nvm_dma_alloc(c, 65536, NULL);
    for (int i = 0; i < 65536; i++)
        w[i] = (uint8_t)(i * 13 + 1);
    EXPECT(nvm_write_sync(iq, ns->nsid, w, 2048, 65536 / ns->lba_size) == 0, "irq write");
    EXPECT(nvm_read_sync(iq, ns->nsid, r, 2048, 65536 / ns->lba_size) == 0, "irq read");
    EXPECT(memcmp(w, r, 65536) == 0, "irq sync data mismatch");
    /* And the polled queue pair on the same controller still works. */
    memset(r, 0, 65536);
    EXPECT(nvm_read_sync(pq, ns->nsid, r, 2048, 65536 / ns->lba_size) == 0, "poll read");
    EXPECT(memcmp(w, r, 65536) == 0, "poll read sees different data");

    nvm_dma_free(c, w);
    nvm_dma_free(c, r);
    nvm_dma_free(c, buf);
    nvm_qpair_free(iq);
    nvm_qpair_free(pq);
    nvm_ctrl_close(c);
    printf("%s (%d failures)\n", failures ? "FAIL" : "OK", failures);
    return failures != 0;
}
