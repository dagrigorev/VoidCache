/*
 * bench/benchmark.c  –  VoidCache Benchmark Suite
 *
 * Scenarios:
 *   1. Single-threaded SET throughput
 *   2. Single-threaded GET throughput (hot 100% hit)
 *   3. Single-threaded GET throughput (zipf distribution)
 *   4. Multi-threaded SET × 2/4/8/16
 *   5. Multi-threaded GET × 2/4/8/16
 *   6. Mixed 80% GET / 20% SET (real-world ratio)
 *   7. Small key+value (8B key, 8B val) – inline fast path
 *   8. Medium values (256B)
 *   9. Large values (4096B, slab path)
 *  10. Estimated comparison vs Redis/Memcached (based on
 *      published throughput numbers)
 */

#define _POSIX_C_SOURCE 200809L
#include "voidcache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>

/* ── timing ──────────────────────────────────────────────── */
static inline uint64_t ns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── output ──────────────────────────────────────────────── */
static void hdr(void) {
    printf("\n%-48s  %10s  %10s  %8s\n",
           "Benchmark", "ops", "Mops/sec", "ns/op");
    printf("%-48s  %10s  %10s  %8s\n",
           "────────────────────────────────────────────────",
           "──────────", "──────────", "────────");
}

static void row(const char *name, uint64_t ops, uint64_t ns) {
    double mops = (double)ops / (double)ns * 1000.0;
    double nsop = (double)ns  / (double)ops;
    printf("%-48s  %10" PRIu64 "  %10.3f  %8.1f\n", name, ops, mops, nsop);
}

/* ── key generation ──────────────────────────────────────── */
/* Pre-generate random keys to avoid snprintf overhead in hot loop */
#define NKEYS (1 << 20)   /* 1M unique keys */
static char   g_keys[NKEYS][24];
static uint8_t g_vals[4096];

static void gen_keys(void) {
    for (int i = 0; i < NKEYS; i++)
        snprintf(g_keys[i], sizeof(g_keys[i]), "k%010d", i);
    for (int i = 0; i < 4096; i++)
        g_vals[i] = (uint8_t)(i ^ 0xA5);
}

/* Zipf distribution (s=1.0) for realistic GET patterns */
static int zipf_table[NKEYS];
static void build_zipf(void) {
    /* Build CDF for top 1024 ranks (approx) */
    double sum = 0;
    for (int i = 1; i <= 1024; i++) sum += 1.0 / i;
    double cumul = 0;
    int idx = 0;
    for (int rank = 1; rank <= 1024 && idx < NKEYS; rank++) {
        double frac = (1.0 / rank) / sum;
        int count = (int)(frac * NKEYS);
        for (int j = 0; j < count && idx < NKEYS; j++)
            zipf_table[idx++] = rank - 1;
    }
    while (idx < NKEYS) zipf_table[idx++] = 0;
}

/* ══════════════════════════════════════════════════════════
 * SINGLE-THREAD BENCHMARKS
 * ══════════════════════════════════════════════════════════ */

static void bench_set(vc_cache_t *c, size_t vlen, int n, const char *label) {
    uint64_t t0 = ns_now();
    for (int i = 0; i < n; i++) {
        int k = i & (NKEYS - 1);
        vc_set(c, g_keys[k], 12, g_vals, vlen, 0);
    }
    row(label, (uint64_t)n, ns_now() - t0);
}

static void bench_get_hot(vc_cache_t *c, size_t vlen, int n, const char *label) {
    /* pre-populate */
    for (int i = 0; i < NKEYS; i++)
        vc_set(c, g_keys[i], 12, g_vals, vlen, 0);

    uint64_t t0 = ns_now();
    size_t hits = 0;
    for (int i = 0; i < n; i++) {
        int k = i & (NKEYS - 1);
        const void *v; size_t vl;
        if (vc_get(c, g_keys[k], 12, &v, &vl) == VC_OK) hits++;
    }
    (void)hits;
    row(label, (uint64_t)n, ns_now() - t0);
}

static void bench_get_zipf(vc_cache_t *c, int n, const char *label) {
    uint64_t t0 = ns_now();
    for (int i = 0; i < n; i++) {
        int k = zipf_table[i & (NKEYS - 1)];
        const void *v; size_t vl;
        vc_get(c, g_keys[k], 12, &v, &vl);
    }
    row(label, (uint64_t)n, ns_now() - t0);
}

/* ══════════════════════════════════════════════════════════
 * MULTI-THREAD BENCHMARKS
 * ══════════════════════════════════════════════════════════ */

typedef struct {
    vc_cache_t *cache;
    int         thread_id;
    int         total_threads;
    int         iters;
    int         read_pct;    /* 0-100 */
    size_t      vlen;
    uint64_t    elapsed_ns;
    uint64_t    ops_done;
} mt_arg_t;

static void *mt_worker(void *arg) {
    mt_arg_t *a   = (mt_arg_t *)arg;
    vc_cache_t *c = a->cache;
    int         n = a->iters;
    int         rp= a->read_pct;
    size_t      vl= a->vlen;
    int         tid = a->thread_id;

    uint64_t t0 = ns_now();
    uint64_t ops = 0;

    for (int i = 0; i < n; i++) {
        /* Each thread works on its own slice + shared keys */
        int local = (i + tid * (NKEYS / a->total_threads)) & (NKEYS - 1);
        if ((i % 100) < rp) {
            const void *v; size_t vlen2;
            vc_get(c, g_keys[local], 12, &v, &vlen2);
        } else {
            vc_set(c, g_keys[local], 12, g_vals, vl, 0);
        }
        ops++;
    }

    a->elapsed_ns = ns_now() - t0;
    a->ops_done   = ops;
    return NULL;
}

static void bench_mt(vc_cache_t *c, int nthreads, int read_pct,
                     size_t vlen, int iters_per_thread, const char *label) {
    pthread_t threads[16];
    mt_arg_t  args[16];
    if (nthreads > 16) nthreads = 16;

    for (int i = 0; i < nthreads; i++) {
        args[i].cache          = c;
        args[i].thread_id      = i;
        args[i].total_threads  = nthreads;
        args[i].iters          = iters_per_thread;
        args[i].read_pct       = read_pct;
        args[i].vlen           = vlen;
        pthread_create(&threads[i], NULL, mt_worker, &args[i]);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(threads[i], NULL);

    uint64_t total_ops = 0, max_ns = 0;
    for (int i = 0; i < nthreads; i++) {
        total_ops += args[i].ops_done;
        if (args[i].elapsed_ns > max_ns) max_ns = args[i].elapsed_ns;
    }
    row(label, total_ops, max_ns);
}

/* ══════════════════════════════════════════════════════════
 * COMPARISON TABLE
 * Reference numbers from public benchmarks (ops/sec):
 *   Redis    single-thread GET ≈  0.8M  (loopback TCP)
 *            pipelining ×16     ≈  6M
 *   Memcached multi-thread GET ≈  2.0M  (8 threads, loopback)
 * VoidCache numbers are live measurements from this run.
 * ══════════════════════════════════════════════════════════ */

static void print_comparison(double vc_get_mops, double vc_set_mops,
                             double vc_mt_mops) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║           Comparison vs Redis & Memcached                ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║ %-28s  %8s  %8s  %8s ║\n", "Scenario","VoidCache","Redis","Memcached");
    printf("║ %-28s  %8s  %8s  %8s ║\n",
           "────────────────────────────","─────────","─────","─────────");

    /* Redis single-thread: ~0.8 Mops GET, ~0.75 Mops SET (loopback) */
    double r_get = 0.80, r_set = 0.75;
    /* Memcached 8-thread: ~2.0 Mops GET, ~1.5 Mops SET */
    double m_get = 2.00, m_set = 1.50;
    /* Pipelined Redis (pipeline depth 16): ~6 Mops */
    double r_pip = 6.00;

    printf("║ %-28s  %7.2fM  %7.2fM  %7.2fM ║\n",
           "Single-thread GET", vc_get_mops, r_get, m_get);
    printf("║ %-28s  %7.2fM  %7.2fM  %7.2fM ║\n",
           "Single-thread SET", vc_set_mops, r_set, m_set);
    printf("║ %-28s  %7.2fM  %7.2fM  %7.2fM ║\n",
           "Multi-thread (8T) GET", vc_mt_mops, r_pip, m_get);

    double get_vs_redis = vc_get_mops / r_get;
    double get_vs_memc  = vc_get_mops / m_get;
    double mt_vs_redis  = vc_mt_mops  / r_pip;
    double mt_vs_memc   = vc_mt_mops  / m_get;

    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  VoidCache vs Redis:     GET ×%.1f    8T-GET ×%.1f         ║\n",
           get_vs_redis, mt_vs_redis);
    printf("║  VoidCache vs Memcached: GET ×%.1f    8T-GET ×%.1f         ║\n",
           get_vs_memc, mt_vs_memc);
    printf("╚══════════════════════════════════════════════════════════╝\n");
}

/* ══════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════ */
int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║               VoidCache Benchmark Suite                  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    gen_keys();
    build_zipf();

    /* Large shard table: 64 shards × 32768 slots = 2M entries total */
    vc_cache_t *c = vc_create(2ULL * 1024 * 1024 * 1024, NULL, 32768);
    if (!c) { fprintf(stderr, "vc_create failed\n"); return 1; }

    const int ST  = 2000000;   /* single-thread iterations */
    const int MT  = 500000;    /* per-thread iterations in MT tests */

    /* ── warm up ─────────────────────────────────────────── */
    for (int i = 0; i < 50000; i++)
        vc_set(c, g_keys[i & (NKEYS-1)], 12, g_vals, 8, 0);

    /* ── section 1: inline fast path (8B val) ───────────── */
    hdr();
    printf("\n── Inline path  (key=12B, val=8B, key+val fits in cache line)\n");
    bench_set(c, 8, ST, "SET  inline  single-thread");
    bench_get_hot(c, 8, ST, "GET  inline  single-thread  100% hit");
    bench_get_zipf(c, ST, "GET  inline  single-thread  zipf");

    /* ── section 2: slab path (256B val) ────────────────── */
    vc_flush(c);
    printf("\n── Slab path  (key=12B, val=256B)\n");
    bench_set(c, 256, ST/2, "SET  slab    single-thread");
    bench_get_hot(c, 256, ST/2, "GET  slab    single-thread  100% hit");

    /* ── section 3: slab path (4096B val) ───────────────── */
    vc_flush(c);
    printf("\n── Slab path  (key=12B, val=4096B)\n");
    bench_set(c, 4096, ST/8, "SET  slab4k  single-thread");
    bench_get_hot(c, 4096, ST/8, "GET  slab4k  single-thread  100% hit");

    /* ── section 4: multi-threaded GET ──────────────────── */
    vc_flush(c);
    /* pre-populate for GET tests */
    for (int i = 0; i < NKEYS; i++)
        vc_set(c, g_keys[i], 12, g_vals, 8, 0);
    printf("\n── Multi-threaded GET (inline, 100%% reads)\n");
    bench_mt(c, 1,  100, 8, MT*4,    "GET×1   thread");
    bench_mt(c, 2,  100, 8, MT*2,    "GET×2   threads");
    bench_mt(c, 4,  100, 8, MT,      "GET×4   threads");
    bench_mt(c, 8,  100, 8, MT,      "GET×8   threads");

    /* ── section 5: multi-threaded SET ──────────────────── */
    printf("\n── Multi-threaded SET (inline)\n");
    bench_mt(c, 1,  0, 8, MT*4,    "SET×1   thread");
    bench_mt(c, 2,  0, 8, MT*2,    "SET×2   threads");
    bench_mt(c, 4,  0, 8, MT,      "SET×4   threads");
    bench_mt(c, 8,  0, 8, MT,      "SET×8   threads");

    /* ── section 6: realistic 80/20 mix ─────────────────── */
    printf("\n── 80%% GET / 20%% SET mixed workload\n");
    bench_mt(c, 1,  80, 8, MT*4,   "MIXED×1  thread");
    bench_mt(c, 2,  80, 8, MT*2,   "MIXED×2  threads");
    bench_mt(c, 4,  80, 8, MT,     "MIXED×4  threads");
    bench_mt(c, 8,  80, 8, MT,     "MIXED×8  threads");

    /* ── capture key numbers for comparison table ────────── */
    /* Run fresh single-thread GET to get a clean number */
    vc_flush(c);
    for (int i = 0; i < NKEYS; i++)
        vc_set(c, g_keys[i], 12, g_vals, 8, 0);

    uint64_t t0 = ns_now();
    for (int i = 0; i < ST; i++) {
        const void *v; size_t vl;
        vc_get(c, g_keys[i & (NKEYS-1)], 12, &v, &vl);
    }
    double vc_get_mops = (double)ST / ((double)(ns_now()-t0) / 1e3);

    t0 = ns_now();
    for (int i = 0; i < ST; i++)
        vc_set(c, g_keys[i & (NKEYS-1)], 12, g_vals, 8, 0);
    double vc_set_mops = (double)ST / ((double)(ns_now()-t0) / 1e3);

    /* 8-thread GET */
    pthread_t threads[8]; mt_arg_t args[8];
    for (int i = 0; i < 8; i++) {
        args[i].cache = c; args[i].thread_id = i; args[i].total_threads = 8;
        args[i].iters = MT; args[i].read_pct = 100; args[i].vlen = 8;
        pthread_create(&threads[i], NULL, mt_worker, &args[i]);
    }
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);
    uint64_t total8 = 0, max8ns = 0;
    for (int i = 0; i < 8; i++) {
        total8  += args[i].ops_done;
        if (args[i].elapsed_ns > max8ns) max8ns = args[i].elapsed_ns;
    }
    double vc_mt_mops = (double)total8 / ((double)max8ns / 1e3);

    printf("\n");
    vc_stats_print(c);
    print_comparison(vc_get_mops, vc_set_mops, vc_mt_mops);

    vc_destroy(c);
    return 0;
}
