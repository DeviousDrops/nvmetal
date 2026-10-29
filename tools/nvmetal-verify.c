/*
 * nvmetal-verify: end-to-end data verification through nvmetal.
 *
 * Default mode: N threads, each with its own queue pair and its own slice of
 * the namespace. Each pass writes randomly sized (4 KiB .. max transfer),
 * randomly ordered, non-overlapping extents, flushes, then reads the whole
 * slice back with a different random extent layout and checks every 4 KiB
 * block's header: magic, run id, LBA, sequence number and a CRC-32C of the
 * block. That catches corruption (CRC), misdirected writes (LBA), lost or
 * stale writes (sequence) and writes leaking in from elsewhere (run id).
 *
 * Other modes, for the kernel cross-check and error-path tests:
 *   --write-pattern / --dump   deterministic bytes over [--offset, +--length)
 *   --gen-pattern              the same bytes to stdout, no device
 *   --oob                      reads and writes past the namespace end
 */
#include "crc32c.h"
#include "nvmetal/nvmetal.h"

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UNIT       4096u                 /* verification block */
#define MAGIC      0x6e766d6574616c21ull /* "nvmetal!" */
#define MAX_ERRORS 10

struct vhdr {
    uint64_t magic;
    uint64_t run_id;
    uint64_t lba;
    uint64_t seq;
    uint32_t thread;
    uint32_t crc; /* CRC-32C of the whole block with this field zero */
};

static struct {
    const char *bdf;
    uint64_t bytes;
    int threads;
    int qd;
    uint32_t min_bs, max_bs;
    uint64_t seed;
    uint64_t region_bytes; /* 0 = whole namespace */
    uint64_t offset, length;
    int json;
    int inject; /* corrupted blocks planted per pass, to prove detection works */
    int mode;   /* 0 verify, 1 write-pattern, 2 dump, 3 gen-pattern, 4 oob */
} opt = {.bytes = 1ull << 30, .threads = 4, .qd = 32, .min_bs = 4096, .seed = 1};

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

struct rng {
    uint64_t s;
};

static uint64_t rnd(struct rng *r)
{
    r->s = splitmix64(r->s);
    return r->s;
}

static uint64_t rnd_range(struct rng *r, uint64_t lo, uint64_t hi) /* inclusive */
{
    return lo + rnd(r) % (hi - lo + 1);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- Verify mode ---- */

struct extent {
    uint64_t unit; /* first 4 KiB unit, relative to the thread's slice */
    uint32_t n;
};

struct slot {
    uint8_t *buf;
    struct extent ex;
    struct thr *t;
    int is_write;
};

struct thr {
    int id;
    pthread_t tid;
    struct nvm_ctrl *c;
    struct nvm_qpair *qp;
    const struct nvm_ns_info *ns;
    uint64_t run_id;
    uint64_t unit0, nunits;
    uint64_t *expect; /* sequence written to each unit in this run, 0 = never */
    uint64_t seq;
    struct rng rng;
    struct slot *slots;
    int *free_slots;
    int nfree;
    uint64_t done;
    /* results */
    uint64_t bytes_written, bytes_verified, blocks_checked, commands;
    uint64_t mismatches, io_errors, passes, injected;
    int fatal;
};

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;

static void report(struct thr *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void report(struct thr *t, const char *fmt, ...)
{
    if (t->mismatches + t->io_errors > MAX_ERRORS)
        return;
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&report_lock);
    fprintf(stderr, "thread %d: ", t->id);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&report_lock);
    va_end(ap);
}

static void fill_block(uint8_t *b, const struct thr *t, uint64_t lba, uint64_t seq)
{
    struct rng r = {t->run_id ^ splitmix64(lba) ^ (seq << 40)};
    for (size_t i = sizeof(struct vhdr); i < UNIT; i += 8) {
        uint64_t v = rnd(&r);
        memcpy(b + i, &v, 8);
    }
    struct vhdr h = {MAGIC, t->run_id, lba, seq, (uint32_t)t->id, 0};
    h.crc = crc32c(crc32c(0, &h, sizeof(h)), b + sizeof(h), UNIT - sizeof(h));
    memcpy(b, &h, sizeof(h));
}

static void check_block(struct thr *t, const uint8_t *b, uint64_t unit)
{
    uint64_t lba = (t->unit0 + unit) * (UNIT / t->ns->lba_size);
    uint64_t want = t->expect[unit];
    struct vhdr h;
    memcpy(&h, b, sizeof(h));
    if (want == 0) {
        /* Never written in this run: it must not look like this run wrote it. */
        if (h.magic == MAGIC && h.run_id == t->run_id) {
            t->mismatches++;
            report(t,
                   "lba %" PRIu64 ": holds this run's data (lba %" PRIu64 " seq %" PRIu64
                   ") but was never written: misdirected write",
                   lba, h.lba, h.seq);
        }
        return;
    }
    t->blocks_checked++;
    t->bytes_verified += UNIT;
    struct vhdr z = h;
    z.crc = 0;
    uint32_t crc = crc32c(crc32c(0, &z, sizeof(z)), b + sizeof(z), UNIT - sizeof(z));
    const char *why = NULL;
    if (h.magic != MAGIC || h.run_id != t->run_id)
        why = "header missing";
    else if (crc != h.crc)
        why = "crc mismatch";
    else if (h.lba != lba)
        why = "wrong lba (misdirected)";
    else if (h.seq != want)
        why = "stale sequence (lost write)";
    if (why) {
        t->mismatches++;
        report(t,
               "lba %" PRIu64 ": %s (found lba %" PRIu64 " seq %" PRIu64 ", want seq %" PRIu64 ")",
               lba, why, h.lba, h.seq, want);
    }
}

static void io_done(void *arg, const struct nvm_cpl *cpl)
{
    struct slot *s = arg;
    struct thr *t = s->t;
    if (nvm_status_is_error(cpl->status)) {
        char b[96];
        t->io_errors++;
        report(t, "%s of %u units at unit %" PRIu64 " failed: %s", s->is_write ? "write" : "read",
               s->ex.n, t->unit0 + s->ex.unit, nvm_status_str(cpl->status, b, sizeof(b)));
    } else if (s->is_write) {
        for (uint32_t i = 0; i < s->ex.n; i++)
            t->expect[s->ex.unit + i] = t->seq;
        t->bytes_written += (uint64_t)s->ex.n * UNIT;
    } else {
        for (uint32_t i = 0; i < s->ex.n; i++)
            check_block(t, s->buf + (size_t)i * UNIT, s->ex.unit + i);
    }
    t->free_slots[t->nfree++] = (int)(s - t->slots);
    t->done++;
}

/* Random partition of [0, nunits) into extents of min..max units, shuffled. */
static struct extent *partition(struct thr *t, size_t *count)
{
    uint32_t lo = opt.min_bs / UNIT, hi = opt.max_bs / UNIT;
    size_t cap = t->nunits / lo + 1, n = 0;
    struct extent *ex = malloc(cap * sizeof(*ex));
    for (uint64_t u = 0; u < t->nunits;) {
        uint32_t len = (uint32_t)rnd_range(&t->rng, lo, hi);
        if (u + len > t->nunits)
            len = (uint32_t)(t->nunits - u);
        ex[n++] = (struct extent){u, len};
        u += len;
    }
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rnd(&t->rng) % (i + 1);
        struct extent tmp = ex[i];
        ex[i] = ex[j];
        ex[j] = tmp;
    }
    *count = n;
    return ex;
}

static int run_extents(struct thr *t, const struct extent *ex, size_t n, int is_write)
{
    uint32_t per_lba = UNIT / t->ns->lba_size;
    size_t next = 0;
    t->done = 0;
    while (t->done < n) {
        while (next < n && t->nfree > 0) {
            int si = t->free_slots[t->nfree - 1];
            struct slot *s = &t->slots[si];
            s->ex = ex[next];
            s->is_write = is_write;
            uint64_t lba = (t->unit0 + s->ex.unit) * per_lba;
            uint32_t nlb = s->ex.n * per_lba;
            int rc;
            if (is_write) {
                for (uint32_t i = 0; i < s->ex.n; i++)
                    fill_block(s->buf + (size_t)i * UNIT, t, lba + (uint64_t)i * per_lba, t->seq);
                rc = nvm_write(t->qp, t->ns->nsid, s->buf, lba, nlb, io_done, s);
            } else {
                rc = nvm_read(t->qp, t->ns->nsid, s->buf, lba, nlb, io_done, s);
            }
            if (rc == -EAGAIN)
                break;
            if (rc) {
                report(t, "submit failed: %s", strerror(-rc));
                return rc;
            }
            t->nfree--;
            t->commands++;
            next++;
        }
        nvm_qpair_poll(t->qp, (uint32_t)opt.qd);
    }
    return 0;
}

/*
 * Self-test: overwrite a few written blocks behind the verifier's back, each
 * with a different defect, without updating the expected sequence. The read
 * phase must then report exactly these blocks.
 */
static void inject_corruption(struct thr *t)
{
    uint8_t *b = t->slots[0].buf;
    uint32_t per_lba = UNIT / t->ns->lba_size;
    for (int i = 0; i < opt.inject; i++) {
        uint64_t u;
        do
            u = rnd(&t->rng) % t->nunits;
        while (t->expect[u] == 0);
        uint64_t lba = (t->unit0 + u) * per_lba;
        switch (i % 3) {
        case 0: /* bit flip after the CRC was computed */
            fill_block(b, t, lba, t->expect[u]);
            b[UNIT - 1] ^= 0x01;
            break;
        case 1: /* another LBA's block: a misdirected write */
            fill_block(b, t, lba + per_lba, t->expect[u]);
            break;
        default: /* an older version of the block: a lost write */
            fill_block(b, t, lba, t->expect[u] + 1000);
            break;
        }
        if (nvm_write_sync(t->qp, t->ns->nsid, b, lba, per_lba) == 0)
            t->injected++;
    }
}

static void *verify_thread(void *arg)
{
    struct thr *t = arg;
    struct nvm_qpair_opts qo = {.depth = (uint32_t)opt.qd + 1};
    int rc = nvm_qpair_alloc(t->c, &qo, &t->qp);
    if (rc) {
        report(t, "qpair alloc: %s", strerror(-rc));
        t->fatal = 1;
        return NULL;
    }
    uint64_t target = opt.bytes / (uint64_t)opt.threads;
    /* Each pass writes about half the slice, so passes leave stale-looking
     * older data around for the sequence check to catch. */
    uint64_t pass_bytes = t->nunits * UNIT / 2;
    if (pass_bytes > target)
        pass_bytes = target;
    while (t->bytes_written < target && !t->fatal) {
        t->seq++;
        size_t n;
        struct extent *ex = partition(t, &n);
        size_t k = 0;
        uint64_t sum = 0;
        while (k < n && sum < pass_bytes)
            sum += (uint64_t)ex[k++].n * UNIT;
        if (run_extents(t, ex, k, 1) || nvm_flush_sync(t->qp, t->ns->nsid) != 0)
            t->fatal = 1;
        free(ex);
        if (opt.inject && !t->fatal)
            inject_corruption(t);
        ex = partition(t, &n);
        if (!t->fatal && run_extents(t, ex, n, 0))
            t->fatal = 1;
        free(ex);
        t->passes++;
        if ((t->mismatches || t->io_errors) && !opt.inject)
            break;
    }
    nvm_qpair_free(t->qp);
    return NULL;
}

static int mode_verify(struct nvm_ctrl *c, const struct nvm_ns_info *ns)
{
    uint64_t ns_bytes = ns->nlba * ns->lba_size;
    uint64_t region = opt.region_bytes ? opt.region_bytes : ns_bytes;
    if (region > ns_bytes)
        region = ns_bytes;
    uint64_t units_per_thread = region / UNIT / (uint64_t)opt.threads;
    if (UNIT % ns->lba_size || units_per_thread < opt.max_bs / UNIT) {
        fprintf(stderr, "namespace too small or LBA size %u unsupported\n", ns->lba_size);
        return 2;
    }
    uint64_t run_id = splitmix64(opt.seed ^ (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32));
    struct thr *th = calloc((size_t)opt.threads, sizeof(*th));
    double t0 = now_s();
    for (int i = 0; i < opt.threads; i++) {
        struct thr *t = &th[i];
        t->id = i;
        t->c = c;
        t->ns = ns;
        t->run_id = run_id;
        t->unit0 = (uint64_t)i * units_per_thread;
        t->nunits = units_per_thread;
        t->rng.s = splitmix64(opt.seed * 1000003u + (uint64_t)i);
        t->expect = calloc(t->nunits, sizeof(*t->expect));
        t->slots = calloc((size_t)opt.qd, sizeof(*t->slots));
        t->free_slots = calloc((size_t)opt.qd, sizeof(int));
        for (int s = 0; s < opt.qd; s++) {
            t->slots[s].buf = nvm_dma_alloc(c, opt.max_bs, NULL);
            t->slots[s].t = t;
            if (!t->slots[s].buf) {
                fprintf(stderr, "DMA pool too small: raise NVMETAL_POOL_MB\n");
                return 2;
            }
            t->free_slots[t->nfree++] = s;
        }
        pthread_create(&t->tid, NULL, verify_thread, t);
    }
    uint64_t w = 0, v = 0, blocks = 0, cmds = 0, mism = 0, ioerr = 0, cmds_min = UINT64_MAX;
    uint64_t injected = 0;
    int fatal = 0;
    for (int i = 0; i < opt.threads; i++) {
        struct thr *t = &th[i];
        pthread_join(t->tid, NULL);
        w += t->bytes_written;
        v += t->bytes_verified;
        blocks += t->blocks_checked;
        cmds += t->commands;
        mism += t->mismatches;
        ioerr += t->io_errors;
        injected += t->injected;
        fatal |= t->fatal;
        if (t->commands < cmds_min)
            cmds_min = t->commands;
        for (int s = 0; s < opt.qd; s++)
            nvm_dma_free(c, t->slots[s].buf);
        free(t->slots);
        free(t->free_slots);
        free(t->expect);
    }
    double secs = now_s() - t0;
    int ok = !fatal && mism == 0 && ioerr == 0 && w >= opt.bytes;
    if (opt.inject) {
        /* Self-test passes only if every planted defect, and nothing else, was caught. */
        ok = !fatal && ioerr == 0 && injected > 0 && mism == injected;
        fprintf(stderr, "injected %" PRIu64 " corrupt blocks, detected %" PRIu64 "\n", injected,
                mism);
    }
    if (opt.json) {
        printf("{\"bdf\":\"%s\",\"threads\":%d,\"qd\":%d,\"min_bs\":%u,\"max_bs\":%u,"
               "\"bytes_written\":%" PRIu64 ",\"bytes_verified\":%" PRIu64
               ",\"blocks_checked\":%" PRIu64 ",\"commands\":%" PRIu64
               ",\"min_commands_per_thread\":%" PRIu64 ",\"queue_depth_multiple\":%.1f,"
               "\"mismatches\":%" PRIu64 ",\"io_errors\":%" PRIu64 ",\"seconds\":%.2f,"
               "\"result\":\"%s\"}\n",
               opt.bdf, opt.threads, opt.qd, opt.min_bs, opt.max_bs, w, v, blocks, cmds, cmds_min,
               (double)cmds_min / (opt.qd + 1), mism, ioerr, secs, ok ? "pass" : "fail");
    } else {
        printf("written %.2f GiB, verified %.2f GiB (%" PRIu64 " blocks), %" PRIu64
               " commands (min %" PRIu64 "/thread = %.0fx queue depth), %" PRIu64
               " mismatches, %" PRIu64 " I/O errors, %.1f s: %s\n",
               w / 1073741824.0, v / 1073741824.0, blocks, cmds, cmds_min,
               (double)cmds_min / (opt.qd + 1), mism, ioerr, secs, ok ? "PASS" : "FAIL");
    }
    free(th);
    return ok ? 0 : 1;
}

/* ---- Pattern, dump and out-of-range modes ---- */

static void pattern_fill(uint8_t *p, uint64_t abs_off, size_t len)
{
    /* Each 8-byte word is a function of its absolute byte offset, so the
     * stream is identical however it is chunked. */
    for (size_t i = 0; i < len; i += 8) {
        uint64_t v = splitmix64(opt.seed ^ ((abs_off + i) / 8));
        memcpy(p + i, &v, 8);
    }
}

static int check_range(const struct nvm_ns_info *ns)
{
    if (opt.offset % 4096 || opt.length % 4096 || !opt.length ||
        opt.offset + opt.length > ns->nlba * ns->lba_size) {
        fprintf(stderr, "--offset/--length must be 4 KiB multiples inside the namespace\n");
        return -1;
    }
    return 0;
}

static int mode_pattern_io(struct nvm_ctrl *c, const struct nvm_ns_info *ns, int dump)
{
    if (check_range(ns))
        return 2;
    struct nvm_qpair *qp;
    if (nvm_qpair_alloc(c, NULL, &qp))
        return 1;
    uint32_t chunk = opt.max_bs;
    uint8_t *buf = nvm_dma_alloc(c, chunk, NULL);
    int rc = 0;
    for (uint64_t off = 0; off < opt.length && !rc; off += chunk) {
        uint32_t len = (uint32_t)(opt.length - off < chunk ? opt.length - off : chunk);
        uint64_t lba = (opt.offset + off) / ns->lba_size;
        uint32_t nlb = len / ns->lba_size;
        if (dump) {
            rc = nvm_read_sync(qp, ns->nsid, buf, lba, nlb);
            if (!rc && fwrite(buf, 1, len, stdout) != len)
                rc = -EIO;
        } else {
            pattern_fill(buf, opt.offset + off, len);
            rc = nvm_write_sync(qp, ns->nsid, buf, lba, nlb);
        }
    }
    if (!dump && !rc)
        rc = nvm_flush_sync(qp, ns->nsid);
    if (rc) {
        char b[96];
        fprintf(stderr, "%s failed: %s\n", dump ? "dump" : "write-pattern",
                rc > 0 ? nvm_status_str((uint32_t)rc, b, sizeof(b)) : strerror(-rc));
    } else if (!dump) {
        fprintf(stderr,
                "wrote %" PRIu64 " bytes of pattern seed %" PRIu64 " at offset %" PRIu64 "\n",
                opt.length, opt.seed, opt.offset);
    }
    nvm_dma_free(c, buf);
    nvm_qpair_free(qp);
    return rc ? 1 : 0;
}

static int mode_gen_pattern(void)
{
    static uint8_t buf[1 << 20];
    for (uint64_t off = 0; off < opt.length; off += sizeof(buf)) {
        size_t len = opt.length - off < sizeof(buf) ? opt.length - off : sizeof(buf);
        pattern_fill(buf, opt.offset + off, len);
        if (fwrite(buf, 1, len, stdout) != len)
            return 1;
    }
    return 0;
}

static int mode_oob(struct nvm_ctrl *c, const struct nvm_ns_info *ns)
{
    /* Commands that touch LBAs at or past NSZE must fail with LBA Out of
     * Range (§6.9, §6.15) and leave the queue pair fully usable. */
    struct nvm_qpair *qp;
    if (nvm_qpair_alloc(c, NULL, &qp))
        return 1;
    uint8_t *buf = nvm_dma_alloc(c, 8192, NULL);
    struct {
        const char *what;
        int write;
        uint64_t lba;
        uint32_t nlb;
    } cases[] = {
        {"read at NSZE", 0, ns->nlba, 1},
        {"read straddling the end", 0, ns->nlba - 1, 2},
        {"read far past the end", 0, ns->nlba + 1000000, 1},
        {"write at NSZE", 1, ns->nlba, 1},
    };
    int bad = 0;
    char b[96];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int rc = cases[i].write ? nvm_write_sync(qp, ns->nsid, buf, cases[i].lba, cases[i].nlb)
                                : nvm_read_sync(qp, ns->nsid, buf, cases[i].lba, cases[i].nlb);
        int ok = rc > 0 && NVM_STATUS_SCT((uint32_t)rc) == 0 && NVM_STATUS_SC((uint32_t)rc) == 0x80;
        printf("%-26s lba %-10" PRIu64 " -> %s: %s\n", cases[i].what, cases[i].lba,
               rc > 0 ? nvm_status_str((uint32_t)rc, b, sizeof(b))
                      : (rc ? strerror(-rc) : "success"),
               ok ? "ok" : "UNEXPECTED");
        bad += !ok;
    }
    /* The last valid block must still work after the errors. */
    int rc = nvm_read_sync(qp, ns->nsid, buf, ns->nlba - 1, 1);
    printf("%-26s lba %-10" PRIu64 " -> %s\n", "read of last LBA", ns->nlba - 1,
           rc ? "FAILED" : "success");
    bad += rc != 0;
    nvm_dma_free(c, buf);
    nvm_qpair_free(qp);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

static uint64_t parse_size(const char *s)
{
    char *end;
    double v = strtod(s, &end);
    switch (*end) {
    case 'k':
    case 'K':
        v *= 1024;
        break;
    case 'm':
    case 'M':
        v *= 1048576;
        break;
    case 'g':
    case 'G':
        v *= 1073741824.0;
        break;
    default:
        break;
    }
    return (uint64_t)v;
}

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [options] <bdf>\n"
            "  --bytes N       total bytes to write and verify (default 1G)\n"
            "  --threads N     threads, one queue pair each (default 4)\n"
            "  --qd N          queue depth per thread (default 32)\n"
            "  --min-bs N      smallest I/O (default 4K)\n"
            "  --max-bs N      largest I/O (default: controller max transfer)\n"
            "  --region N      bytes of the namespace to use (default: all)\n"
            "  --seed N        random seed\n"
            "  --json          machine readable summary\n"
            "  --write-pattern | --dump  --offset N --length N   pattern I/O\n"
            "  --gen-pattern --offset N --length N                pattern to stdout\n"
            "  --oob           out-of-range error path test\n"
            "  --inject N      self-test: plant N corrupt blocks per pass, expect them caught\n",
            p);
}

int main(int argc, char **argv)
{
    static const struct option lo[] = {
        {"bytes", required_argument, 0, 'b'},
        {"threads", required_argument, 0, 't'},
        {"qd", required_argument, 0, 'q'},
        {"min-bs", required_argument, 0, 'm'},
        {"max-bs", required_argument, 0, 'M'},
        {"region", required_argument, 0, 'r'},
        {"seed", required_argument, 0, 's'},
        {"json", no_argument, 0, 'j'},
        {"offset", required_argument, 0, 'o'},
        {"length", required_argument, 0, 'l'},
        {"write-pattern", no_argument, 0, 1},
        {"dump", no_argument, 0, 2},
        {"gen-pattern", no_argument, 0, 3},
        {"oob", no_argument, 0, 4},
        {"inject", required_argument, 0, 'i'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "h", lo, NULL)) != -1) {
        switch (ch) {
        case 'b':
            opt.bytes = parse_size(optarg);
            break;
        case 't':
            opt.threads = atoi(optarg);
            break;
        case 'q':
            opt.qd = atoi(optarg);
            break;
        case 'm':
            opt.min_bs = (uint32_t)parse_size(optarg);
            break;
        case 'M':
            opt.max_bs = (uint32_t)parse_size(optarg);
            break;
        case 'r':
            opt.region_bytes = parse_size(optarg);
            break;
        case 's':
            opt.seed = strtoull(optarg, NULL, 0);
            break;
        case 'j':
            opt.json = 1;
            break;
        case 'i':
            opt.inject = atoi(optarg);
            break;
        case 'o':
            opt.offset = parse_size(optarg);
            break;
        case 'l':
            opt.length = parse_size(optarg);
            break;
        case 1:
        case 2:
        case 3:
        case 4:
            opt.mode = ch;
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (opt.mode == 3)
        return mode_gen_pattern();
    if (optind != argc - 1 || opt.threads < 1 || opt.threads > 64 || opt.qd < 1 || opt.qd > 1023) {
        usage(argv[0]);
        return 2;
    }
    opt.bdf = argv[optind];

    struct nvm_ctrl *c;
    int rc = nvm_ctrl_open(opt.bdf, NULL, &c);
    if (rc) {
        fprintf(stderr, "open %s: %s\n", opt.bdf, strerror(-rc));
        return 1;
    }
    const struct nvm_ns_info *ns = nvm_ctrl_ns_at(c, 0);
    uint32_t max = nvm_ctrl_info(c)->max_xfer;
    if (!opt.max_bs || opt.max_bs > max)
        opt.max_bs = max;
    opt.min_bs = opt.min_bs < UNIT ? UNIT : opt.min_bs / UNIT * UNIT;
    opt.max_bs = opt.max_bs / UNIT * UNIT;
    if (!ns || opt.min_bs > opt.max_bs) {
        fprintf(stderr, "no namespace, or --min-bs > --max-bs\n");
        nvm_ctrl_close(c);
        return 2;
    }
    switch (opt.mode) {
    case 1:
        rc = mode_pattern_io(c, ns, 0);
        break;
    case 2:
        rc = mode_pattern_io(c, ns, 1);
        break;
    case 4:
        rc = mode_oob(c, ns);
        break;
    default:
        rc = mode_verify(c, ns);
        break;
    }
    nvm_ctrl_close(c);
    return rc;
}
