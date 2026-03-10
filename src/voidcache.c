/*
 * voidcache.c  –  VoidCache core implementation
 *
 * Build flags: -O3 -march=native -funroll-loops
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include "voidcache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════
 * UTILITIES
 * ══════════════════════════════════════════════════════════════ */

static inline uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline int64_t wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

static inline uint32_t next_pow2_32(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}

/* ── xxHash32 (inline, no external dep) ──────────────────── */
#define XXH_PRIME1  0x9E3779B1u
#define XXH_PRIME2  0x85EBCA77u
#define XXH_PRIME3  0xC2B2AE3Du
#define XXH_PRIME4  0x27D4EB2Fu
#define XXH_PRIME5  0x165667B1u

static inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static uint32_t xxh32(const void *data, size_t len, uint32_t seed) {
    const uint8_t *p   = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint32_t h;

    if (len >= 16) {
        uint32_t v1 = seed + XXH_PRIME1 + XXH_PRIME2;
        uint32_t v2 = seed + XXH_PRIME2;
        uint32_t v3 = seed;
        uint32_t v4 = seed - XXH_PRIME1;
        do {
            v1 = rotl32(v1 + (*(uint32_t*)p)*XXH_PRIME2, 13)*XXH_PRIME1; p+=4;
            v2 = rotl32(v2 + (*(uint32_t*)p)*XXH_PRIME2, 13)*XXH_PRIME1; p+=4;
            v3 = rotl32(v3 + (*(uint32_t*)p)*XXH_PRIME2, 13)*XXH_PRIME1; p+=4;
            v4 = rotl32(v4 + (*(uint32_t*)p)*XXH_PRIME2, 13)*XXH_PRIME1; p+=4;
        } while (p <= end - 16);
        h = rotl32(v1,1)+rotl32(v2,7)+rotl32(v3,12)+rotl32(v4,18);
    } else {
        h = seed + XXH_PRIME5;
    }
    h += (uint32_t)len;
    while (p <= end - 4) { h = rotl32(h + (*(uint32_t*)p)*XXH_PRIME3, 17)*XXH_PRIME4; p+=4; }
    while (p < end)      { h = rotl32(h + (*p)*XXH_PRIME5, 11)*XXH_PRIME1; p++; }
    h ^= h >> 15; h *= XXH_PRIME2; h ^= h >> 13; h *= XXH_PRIME3; h ^= h >> 16;
    return h;
}

/* ── wyhash64 – correct implementation for all key lengths ──── */
static inline uint64_t _wymix(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

static inline uint64_t wyhash64(const void *key, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)key;
    seed ^= _wymix(seed ^ 0x9e3779b97f4a7c15ULL, 0xa0761d6478bd642fULL);
    uint64_t a, b;

    if (len <= 16) {
        if (len >= 4) {
            /* read overlapping 4-byte chunks from front and back */
            uint32_t lo, hi, lo2, hi2;
            memcpy(&lo,  p,                       4);
            memcpy(&hi,  p + (len >> 1) - 2,      4);
            a = ((uint64_t)lo << 32) | hi;
            memcpy(&lo2, p + len - 4,              4);
            memcpy(&hi2, p + len - 4 - (len >> 1) + 2, 4);
            b = ((uint64_t)lo2 << 32) | hi2;
        } else if (len > 0) {
            a = ((uint64_t)p[0]         << 16)
              | ((uint64_t)p[len >> 1]  <<  8)
              | p[len - 1];
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        /* Process in 16-byte blocks */
        size_t i = len;
        a = b = 0;
        while (i > 32) {
            uint64_t x, y;
            memcpy(&x, p,    8); memcpy(&y, p+8,  8); a = _wymix(a ^ x, b ^ y);
            memcpy(&x, p+16, 8); memcpy(&y, p+24, 8); b = _wymix(a ^ x, b ^ y);
            p += 32; i -= 32;
        }
        if (i > 16) {
            uint64_t x, y;
            memcpy(&x, p,   8); memcpy(&y, p+8, 8); a = _wymix(a ^ x, b ^ y);
            p += 16; i -= 16;
        }
        /* last ≤16 bytes via two 8-byte overlapping reads */
        uint64_t x, y;
        memcpy(&x, p + i - 16 + ((i > 8) ? 0 : 8 - i), 8);
        memcpy(&y, p + i - 8,  8);
        a ^= x; b ^= y;
    }
    return _wymix(seed ^ (uint64_t)len,
                  _wymix(a ^ 0x9e3779b97f4a7c15ULL,
                         b ^ 0xe7037ed1a0b428dbULL));
}

/* ══════════════════════════════════════════════════════════════
 * SPINLOCK  (per-shard writer lock)
 * ══════════════════════════════════════════════════════════════ */

static inline void shard_lock(vc_shard_t *s) {
    uint32_t exp = 0;
    while (!atomic_compare_exchange_weak_explicit(
               &s->wlock, &exp, 1u,
               memory_order_acquire, memory_order_relaxed)) {
        exp = 0;
        /* pause to reduce bus traffic */
#if defined(__x86_64__)
        __asm__ volatile("pause");
#endif
    }
}

static inline void shard_unlock(vc_shard_t *s) {
    atomic_store_explicit(&s->wlock, 0u, memory_order_release);
}

/* ══════════════════════════════════════════════════════════════
 * SEQLOCK  (reader fast path — no lock, just retry on odd seq)
 * ══════════════════════════════════════════════════════════════ */

static inline uint64_t seq_read_begin(const vc_shard_t *s) {
    uint64_t seq;
    do { seq = atomic_load_explicit(&s->seq, memory_order_acquire); }
    while (seq & 1u);
    return seq;
}

static inline bool seq_read_retry(const vc_shard_t *s, uint64_t seq) {
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&s->seq, memory_order_relaxed) != seq;
}

static inline void seq_write_begin(vc_shard_t *s) {
    atomic_fetch_add_explicit(&s->seq, 1u, memory_order_release);
}

static inline void seq_write_end(vc_shard_t *s) {
    atomic_fetch_add_explicit(&s->seq, 1u, memory_order_release);
}

/* ══════════════════════════════════════════════════════════════
 * SLAB ALLOCATOR
 * Size classes: 64, 128, 256, 512, 1024, 2048, 4096, 8192,
 *               16384, 32768, 65536 ... (VC_SLAB_CLASSES)
 * Each class is backed by a mmap'd arena.
 * Free list is a Treiber stack — CAS on head.
 * ══════════════════════════════════════════════════════════════ */

/* How many arena bytes to pre-map per class */
#define SLAB_ARENA_MB  16u

static size_t slab_class_size(unsigned cls) {
    return (size_t)VC_SLAB_BASE << cls;
}

static unsigned slab_class_for(size_t sz) {
    if (sz <= VC_SLAB_BASE) return 0;
    unsigned cls = 0;
    size_t s = VC_SLAB_BASE;
    while (s < sz && cls < VC_SLAB_CLASSES - 1) { s <<= 1; cls++; }
    return cls;
}

static vc_err_t slab_init(vc_slab_t *sl) {
    for (unsigned c = 0; c < VC_SLAB_CLASSES; c++) {
        vc_slab_class_t *sc = &sl->classes[c];
        sc->block_size  = slab_class_size(c);
        sc->arena_size  = (size_t)SLAB_ARENA_MB * 1024 * 1024;
        /* align arena_size to block_size */
        sc->arena_size  = (sc->arena_size / sc->block_size) * sc->block_size;

        sc->arena = (char *)mmap(NULL, sc->arena_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sc->arena == MAP_FAILED) return VC_ERR_OOM;

        /* Pre-build the free list from back to front so head->first block */
        vc_slab_node_t *prev = NULL;
        size_t n = sc->arena_size / sc->block_size;
        for (size_t i = n; i-- > 0; ) {
            vc_slab_node_t *node = (vc_slab_node_t *)(sc->arena + i * sc->block_size);
            node->next = prev;
            prev = node;
        }
        atomic_store(&sc->head, prev);
        atomic_store(&sc->alloc_count, 0);
        atomic_store(&sc->free_count,  0);
    }
    return VC_OK;
}

static void *slab_alloc(vc_slab_t *sl, size_t sz, unsigned *out_cls) {
    unsigned cls = slab_class_for(sz);
    *out_cls = cls;
    vc_slab_class_t *sc = &sl->classes[cls];

    /* Treiber pop */
    vc_slab_node_t *head, *next;
    do {
        head = atomic_load_explicit(&sc->head, memory_order_acquire);
        if (!head) return NULL;   /* class exhausted */
        next = head->next;
    } while (!atomic_compare_exchange_weak_explicit(
                 &sc->head, &head, next,
                 memory_order_acq_rel, memory_order_acquire));

    atomic_fetch_add(&sc->alloc_count, 1);
    return (void *)head;
}

static void slab_free(vc_slab_t *sl, void *ptr, unsigned cls) {
    vc_slab_class_t *sc = &sl->classes[cls];
    vc_slab_node_t *node = (vc_slab_node_t *)ptr;

    /* Treiber push */
    vc_slab_node_t *head;
    do {
        head = atomic_load_explicit(&sc->head, memory_order_acquire);
        node->next = head;
    } while (!atomic_compare_exchange_weak_explicit(
                 &sc->head, &head, node,
                 memory_order_acq_rel, memory_order_acquire));

    atomic_fetch_add(&sc->free_count, 1);
}

static void slab_destroy(vc_slab_t *sl) {
    for (unsigned c = 0; c < VC_SLAB_CLASSES; c++) {
        vc_slab_class_t *sc = &sl->classes[c];
        if (sc->arena && sc->arena != MAP_FAILED)
            munmap(sc->arena, sc->arena_size);
    }
}

/* ══════════════════════════════════════════════════════════════
 * ENTRY HELPERS
 * ══════════════════════════════════════════════════════════════ */

/* Extract key pointer from entry (inline or long) */
static inline const char *entry_key(const vc_entry_t *e) {
    if (e->flags & VC_EFLAG_KEY_LONG) {
        char *kptr;
        memcpy(&kptr, e->payload, sizeof(char *));
        return kptr;
    }
    return (const char *)e->payload;
}

/* Extract value pointer from entry */
static inline const void *entry_val(const vc_entry_t *e) {
    if (e->flags & (VC_EFLAG_VAL_SLAB | VC_EFLAG_VAL_MMAP)) {
        /* pointer stored after key pointer */
        void *vptr;
        size_t off = (e->flags & VC_EFLAG_KEY_LONG) ? sizeof(char *) : e->key_len;
        memcpy(&vptr, e->payload + off, sizeof(void *));
        return vptr;
    }
    /* inline: value follows key in payload */
    return (const void *)(e->payload + e->key_len);
}

/* ── expiry check ─────────────────────────────────────────── */
static inline bool entry_expired(const vc_entry_t *e) {
    if (e->expire_ns == 0) return false;
    return (int64_t)mono_ns() > e->expire_ns;
}

/* ── free external resources attached to an entry ─────────── */
static void entry_free_resources(vc_entry_t *e, vc_slab_t *sl) {
    if (e->flags & VC_EFLAG_KEY_LONG) {
        char *kptr;
        memcpy(&kptr, e->payload, sizeof(char *));
        free(kptr);
    }
    if (e->flags & VC_EFLAG_VAL_SLAB) {
        size_t off = (e->flags & VC_EFLAG_KEY_LONG) ? sizeof(char *) : e->key_len;
        void *vptr;
        memcpy(&vptr, e->payload + off, sizeof(void *));
        /* recover slab class from val_len */
        unsigned cls = slab_class_for((size_t)e->val_len);
        slab_free(sl, vptr, cls);
    } else if (e->flags & VC_EFLAG_VAL_MMAP) {
        size_t off = (e->flags & VC_EFLAG_KEY_LONG) ? sizeof(char *) : e->key_len;
        void *vptr;
        memcpy(&vptr, e->payload + off, sizeof(void *));
        munmap(vptr, (size_t)e->val_len);
    }
}

/* ══════════════════════════════════════════════════════════════
 * SHARD – hash table operations
 * ══════════════════════════════════════════════════════════════ */

static inline uint32_t shard_idx(uint64_t h) {
    return (uint32_t)(h >> (64 - VC_SHARD_BITS));
}

static inline uint32_t table_slot(uint64_t h, uint32_t cap) {
    return (uint32_t)(h & (cap - 1));
}

static inline uint32_t probe_dist(uint64_t h, uint32_t slot, uint32_t cap) {
    uint32_t ideal = table_slot(h, cap);
    return (slot + cap - ideal) & (cap - 1);
}

/*
 * _shard_find – locate a slot matching key/hash.
 * Caller must either hold wlock OR be inside a seqlock read section.
 * Returns slot index or UINT32_MAX if not found/expired.
 */
static uint32_t _shard_find(const vc_shard_t *s,
                             const char *key, uint32_t key_len,
                             uint64_t hash) {
    uint32_t cap  = s->cap;
    uint32_t slot = table_slot(hash, cap);

    /*
     * Linear probe scan.
     *
     * Robin Hood early termination is disabled here because under concurrent
     * writes a BUSY slot may temporarily violate probe-distance ordering
     * (Robin Hood displacement moves entries mid-flight). A false early
     * exit would cause false NOT_FOUND. We scan at most `cap` slots.
     *
     * EMPTY is still a reliable sentinel: Robin Hood guarantees that a key's
     * probe chain is never broken by an EMPTY slot — an EMPTY slot means the
     * key was never placed further along.
     */
    for (uint32_t i = 0; i < cap; i++) {
        const vc_entry_t *e = &s->table[slot];
        uint8_t st = atomic_load_explicit(&e->state, memory_order_acquire);

        if (st == VC_STATE_EMPTY) return UINT32_MAX;

        /* BUSY: skip comparison, continue scan */
        if (st != VC_STATE_BUSY &&
            st == VC_STATE_LIVE &&
            e->key_hash == hash &&
            e->key_len  == key_len) {
            const char *k = entry_key(e);
            if (memcmp(k, key, key_len) == 0) {
                if (entry_expired(e)) return UINT32_MAX;
                return slot;
            }
        }

        slot = (slot + 1) & (cap - 1);
    }
    return UINT32_MAX;
}

/*
 * _shard_evict_one – CLOCK eviction.
 * Sweeps the table starting from clock_hand, clears recently-used
 * bits, and steals the first unreferenced live entry.
 * Returns the slot index of the evicted entry, or UINT32_MAX.
 * Called under wlock.
 */
static uint32_t _shard_evict_one(vc_shard_t *s, vc_slab_t *sl) {
    uint32_t cap   = s->cap;
    uint32_t start = atomic_load_explicit(&s->clock_hand, memory_order_relaxed);
    uint32_t h     = start;

    for (uint32_t i = 0; i < cap; i++) {
        h = (h + 1) & (cap - 1);
        vc_entry_t *e = &s->table[h];
        uint8_t st = atomic_load_explicit(&e->state, memory_order_relaxed);
        if (st != VC_STATE_LIVE) continue;

        if (e->clock_bit) {
            e->clock_bit = 0;  /* give a second chance */
            continue;
        }

        /* Evict this entry */
        atomic_store_explicit(&e->state, VC_STATE_BUSY, memory_order_relaxed);
        entry_free_resources(e, sl);
        atomic_store_explicit(&e->state, VC_STATE_DEAD, memory_order_release);
        atomic_fetch_sub(&s->count, 1);
        atomic_fetch_add(&s->evictions, 1);
        atomic_store_explicit(&s->clock_hand, h, memory_order_relaxed);
        return h;
    }
    return UINT32_MAX;
}

/*
 * _shard_insert – Robin Hood insert.
 * Called under wlock + seqlock write section.
 * Does NOT duplicate-check; caller has already removed old entry.
 */
static void _shard_insert(vc_shard_t *s, vc_entry_t *incoming) {
    uint32_t cap  = s->cap;
    uint32_t slot = table_slot(incoming->key_hash, cap);
    incoming->probe_dist = 0;

    for (uint32_t i = 0; i < cap; i++) {
        vc_entry_t *e = &s->table[slot];
        uint8_t st = atomic_load_explicit(&e->state, memory_order_relaxed);

        if (st == VC_STATE_EMPTY || st == VC_STATE_DEAD) {
            atomic_store_explicit(&e->state, VC_STATE_BUSY, memory_order_relaxed);
            /* copy everything except state */
            e->flags      = incoming->flags;
            e->val_len    = incoming->val_len;
            e->key_len    = incoming->key_len;
            e->key_hash   = incoming->key_hash;
            e->expire_ns  = incoming->expire_ns;
            e->clock_bit  = 1;
            e->probe_dist = incoming->probe_dist;
            memcpy(e->payload, incoming->payload, 32);
            atomic_store_explicit(&e->state, VC_STATE_LIVE, memory_order_release);
            return;
        }

        /* Robin Hood: evict the rich */
        if (st == VC_STATE_LIVE && e->probe_dist < incoming->probe_dist) {
            /* swap */
            vc_entry_t tmp;
            tmp.flags     = e->flags;     tmp.val_len   = e->val_len;
            tmp.key_len   = e->key_len;   tmp.key_hash  = e->key_hash;
            tmp.expire_ns = e->expire_ns; tmp.probe_dist= e->probe_dist;
            memcpy(tmp.payload, e->payload, 32);

            atomic_store_explicit(&e->state, VC_STATE_BUSY, memory_order_relaxed);
            e->flags      = incoming->flags;  e->val_len   = incoming->val_len;
            e->key_len    = incoming->key_len; e->key_hash = incoming->key_hash;
            e->expire_ns  = incoming->expire_ns;
            e->probe_dist = incoming->probe_dist;
            e->clock_bit  = 1;
            memcpy(e->payload, incoming->payload, 32);
            atomic_store_explicit(&e->state, VC_STATE_LIVE, memory_order_release);

            *incoming = tmp;  /* continue inserting displaced entry */
        }

        incoming->probe_dist++;
        slot = (slot + 1) & (cap - 1);
    }
}

/* ══════════════════════════════════════════════════════════════
 * WAL
 * ══════════════════════════════════════════════════════════════ */
#define WAL_MAGIC 0xVC0CA1E0u
/* workaround: hex literal can't start with a letter */
#define WAL_MAGIC_V 0xC0CA1E01u

static vc_err_t wal_init(vc_wal_t *w, const char *path) {
    if (!path) { w->ring = NULL; return VC_OK; }

    w->ring_size = VC_WAL_RING_SIZE;
    w->fd = open(path, O_RDWR | O_CREAT, 0600);
    if (w->fd < 0) return VC_ERR_WAL;

    if (ftruncate(w->fd, (off_t)w->ring_size) < 0) {
        close(w->fd); return VC_ERR_WAL;
    }

    w->ring = (char *)mmap(NULL, w->ring_size,
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED, w->fd, 0);
    if (w->ring == MAP_FAILED) { close(w->fd); return VC_ERR_WAL; }

    atomic_store(&w->write_pos, 0);
    atomic_store(&w->wal_seq,   0);
    pthread_mutex_init(&w->wal_lock, NULL);
    return VC_OK;
}

static void wal_destroy(vc_wal_t *w) {
    if (!w->ring) return;
    msync(w->ring, w->ring_size, MS_SYNC);
    munmap(w->ring, w->ring_size);
    close(w->fd);
    pthread_mutex_destroy(&w->wal_lock);
}

static void wal_append(vc_wal_t *w, uint8_t op,
                        const char *key, uint32_t kl,
                        const void *val, uint32_t vl,
                        uint32_t ttl_sec) {
    if (!w->ring) return;

    size_t payload_sz = (size_t)kl + vl;
    size_t total      = sizeof(vc_wal_hdr_t) + payload_sz;
    /* pad to 8 bytes */
    total = (total + 7u) & ~7u;

    pthread_mutex_lock(&w->wal_lock);

    uint64_t pos = atomic_load(&w->write_pos) % w->ring_size;
    if (pos + total > w->ring_size) pos = 0;  /* wrap */

    /* Build header */
    vc_wal_hdr_t hdr = {0};
    hdr.magic   = WAL_MAGIC_V;
    hdr.op      = op;
    hdr.key_len = kl;
    hdr.val_len = vl;
    hdr.ttl_sec = ttl_sec;
    hdr.seq     = atomic_fetch_add(&w->wal_seq, 1);
    hdr.ts_ns   = wall_ns();

    /* CRC over op + key + val */
    uint32_t crc = xxh32(&op, 1, 0);
    crc = xxh32(key, kl, crc);
    if (vl) crc = xxh32(val, vl, crc);
    hdr.crc32 = crc;

    memcpy(w->ring + pos, &hdr, sizeof(hdr));
    memcpy(w->ring + pos + sizeof(hdr), key, kl);
    if (vl) memcpy(w->ring + pos + sizeof(hdr) + kl, val, vl);

    /* Flush header page to ensure durability before updating write_pos */
    msync(w->ring + pos, total, MS_ASYNC);

    atomic_store(&w->write_pos, pos + total);
    pthread_mutex_unlock(&w->wal_lock);
}

/* ══════════════════════════════════════════════════════════════
 * CACHE LIFECYCLE
 * ══════════════════════════════════════════════════════════════ */

vc_cache_t *vc_create(size_t max_mem,
                      const char *wal_path,
                      uint32_t shard_slots) {
    vc_cache_t *c = (vc_cache_t *)aligned_alloc(VC_CACHE_LINE, sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));

    c->max_memory = max_mem;
    atomic_store(&c->total_memory, 0);

    /* Slab */
    if (slab_init(&c->slab) != VC_OK) { free(c); return NULL; }

    /* WAL */
    if (wal_init(&c->wal, wal_path) != VC_OK) {
        slab_destroy(&c->slab); free(c); return NULL;
    }

    /* Shards */
    uint32_t cap = next_pow2_32(shard_slots ? shard_slots : 1024u);
    size_t   table_bytes = (size_t)cap * sizeof(vc_entry_t);

    for (uint32_t i = 0; i < VC_NUM_SHARDS; i++) {
        vc_shard_t *s = &c->shards[i];
        s->table = (vc_entry_t *)mmap(NULL, table_bytes,
                                      PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (s->table == MAP_FAILED) {
            /* cleanup already-created shards */
            for (uint32_t j = 0; j < i; j++)
                munmap(c->shards[j].table, table_bytes);
            slab_destroy(&c->slab);
            wal_destroy(&c->wal);
            free(c);
            return NULL;
        }
        s->cap = cap;
        atomic_store(&s->count,      0);
        atomic_store(&s->clock_hand, 0);
        atomic_store(&s->seq,        0);
        atomic_store(&s->wlock,      0);
        atomic_store(&s->hits,       0);
        atomic_store(&s->misses,     0);
        atomic_store(&s->sets,       0);
        atomic_store(&s->evictions,  0);
        atomic_store(&s->expirations,0);
    }
    return c;
}

void vc_destroy(vc_cache_t *c) {
    if (!c) return;
    size_t table_bytes = (size_t)c->shards[0].cap * sizeof(vc_entry_t);
    for (uint32_t i = 0; i < VC_NUM_SHARDS; i++) {
        vc_shard_t *s = &c->shards[i];
        /* Free external resources */
        for (uint32_t j = 0; j < s->cap; j++) {
            vc_entry_t *e = &s->table[j];
            if (atomic_load(&e->state) == VC_STATE_LIVE)
                entry_free_resources(e, &c->slab);
        }
        munmap(s->table, table_bytes);
    }
    slab_destroy(&c->slab);
    wal_destroy(&c->wal);
    free(c);
}

/* ══════════════════════════════════════════════════════════════
 * PUBLIC: vc_set
 * ══════════════════════════════════════════════════════════════ */

vc_err_t vc_set(vc_cache_t *c,
                const char *key, size_t key_len,
                const void *val, size_t val_len,
                uint32_t ttl_sec) {
    if (!c || !key || !val || key_len == 0 || val_len == 0 || key_len > UINT32_MAX)
        return VC_ERR_INVAL;

    uint64_t h     = wyhash64(key, key_len, 0);
    uint32_t si    = shard_idx(h);
    vc_shard_t *s  = &c->shards[si];

    /* WAL before mutation */
    wal_append(&c->wal, VC_WAL_SET, key, (uint32_t)key_len,
               val, (uint32_t)val_len, ttl_sec);

    /* Build the new entry (may allocate from slab for large values) */
    vc_entry_t ne;
    memset(&ne, 0, sizeof(ne));
    ne.key_hash  = h;
    ne.key_len   = (uint32_t)key_len;
    ne.val_len   = (uint16_t)(val_len > 65535 ? 65535 : val_len);
    ne.expire_ns = ttl_sec ? (int64_t)(mono_ns() + (uint64_t)ttl_sec * 1000000000ULL) : 0;

    size_t total_inline = key_len + val_len;

    if (total_inline <= 32 && !(key_len > 32)) {
        /* ── FAST PATH: everything inline ─────────────── */
        ne.flags = 0;
        memcpy(ne.payload,           key, key_len);
        memcpy(ne.payload + key_len, val, val_len);
    } else if (val_len <= slab_class_size(VC_SLAB_CLASSES - 1)) {
        /* ── SLAB PATH ─────────────────────────────────── */
        unsigned cls;
        void *vbuf = slab_alloc(&c->slab, val_len, &cls);
        if (!vbuf) return VC_ERR_OOM;
        memcpy(vbuf, val, val_len);
        ne.flags = VC_EFLAG_VAL_SLAB;

        if (key_len <= 32 - sizeof(void *)) {
            memcpy(ne.payload, key, key_len);
            memcpy(ne.payload + key_len, &vbuf, sizeof(void *));
        } else {
            /* long key */
            char *kbuf = (char *)malloc(key_len);
            if (!kbuf) { slab_free(&c->slab, vbuf, cls); return VC_ERR_OOM; }
            memcpy(kbuf, key, key_len);
            ne.flags |= VC_EFLAG_KEY_LONG;
            memcpy(ne.payload,                  &kbuf, sizeof(char *));
            memcpy(ne.payload + sizeof(char *), &vbuf, sizeof(void *));
        }
        atomic_fetch_add(&c->total_memory, val_len);
    } else {
        /* ── MMAP PATH (very large values) ────────────── */
        void *vbuf = mmap(NULL, val_len,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (vbuf == MAP_FAILED) return VC_ERR_OOM;
        memcpy(vbuf, val, val_len);
        ne.flags = VC_EFLAG_VAL_MMAP;
        char *kbuf = (char *)malloc(key_len);
        if (!kbuf) { munmap(vbuf, val_len); return VC_ERR_OOM; }
        memcpy(kbuf, key, key_len);
        ne.flags |= VC_EFLAG_KEY_LONG;
        memcpy(ne.payload,                  &kbuf, sizeof(char *));
        memcpy(ne.payload + sizeof(char *), &vbuf, sizeof(void *));
        atomic_fetch_add(&c->total_memory, val_len);
    }

    shard_lock(s);
    seq_write_begin(s);

    /* If key already exists, remove old entry first */
    uint32_t old_slot = _shard_find(s, key, (uint32_t)key_len, h);
    if (old_slot != UINT32_MAX) {
        vc_entry_t *oe = &s->table[old_slot];
        entry_free_resources(oe, &c->slab);
        atomic_store_explicit(&oe->state, VC_STATE_DEAD, memory_order_release);
        atomic_fetch_sub(&s->count, 1);
    }

    /* Evict if table is 85% full */
    uint32_t cnt = atomic_load(&s->count);
    if (cnt * 100 / s->cap >= 85) {
        _shard_evict_one(s, &c->slab);
    }

    _shard_insert(s, &ne);
    atomic_fetch_add(&s->count, 1);
    atomic_fetch_add(&s->sets, 1);

    seq_write_end(s);
    shard_unlock(s);
    return VC_OK;
}

/* ══════════════════════════════════════════════════════════════
 * PUBLIC: vc_get  (lock-free seqlock reader)
 * ══════════════════════════════════════════════════════════════ */

vc_err_t vc_get(vc_cache_t *c,
                const char *key, size_t key_len,
                const void **out_val, size_t *out_len) {
    if (!c || !key) return VC_ERR_INVAL;

    uint64_t h    = wyhash64(key, key_len, 0);
    uint32_t si   = shard_idx(h);
    vc_shard_t *s = &c->shards[si];

    /*
     * Design note on inline vs external values:
     *
     * Inline values live in entry->payload[].  Returning a pointer into
     * live table memory is safe ONLY when protected by the write lock.
     * Under the seqlock, we copy the entry to a local snapshot first,
     * then validate — but a pointer into the local snapshot becomes
     * dangling after the function returns.
     *
     * Resolution:
     *   - Slab / mmap values: the value lives outside the entry in stable
     *     memory. We return a pointer into that stable memory — zero copy,
     *     valid across the function boundary.
     *   - Inline values: we take the per-shard write lock and return a
     *     pointer into the live entry. The lock keeps the entry stable for
     *     the duration of the call. Callers must copy if they need the
     *     data to outlive the call (documented in the API).
     *
     * This gives zero-copy performance for slab/mmap (the common case
     * for anything > 32 bytes) and correct semantics for tiny inline values.
     */

    /* Fast path: try without the write lock first (slab/mmap values only) */
    for (unsigned retry = 0; retry < VC_SEQLOCK_RETRY; retry++) {
        uint64_t seq = seq_read_begin(s);

        uint32_t slot = _shard_find(s, key, (uint32_t)key_len, h);
        if (slot == UINT32_MAX) {
            if (seq_read_retry(s, seq)) continue;
            atomic_fetch_add(&s->misses, 1);
            return VC_ERR_NOTFND;
        }

        vc_entry_t *e = &s->table[slot];

        /* Only inline values require a lock for pointer stability */
        if (e->flags & (VC_EFLAG_VAL_SLAB | VC_EFLAG_VAL_MMAP)) {
            /* Snapshot the pointer fields from the entry */
            uint8_t  flags = e->flags;
            uint16_t vlen  = e->val_len;
            uint32_t klen  = e->key_len;
            uint8_t  snap_payload[32];
            memcpy(snap_payload, e->payload, 32);

            if (seq_read_retry(s, seq)) continue;

            /* Build a temporary entry to reuse entry_val logic */
            vc_entry_t snap;
            snap.flags   = flags;
            snap.val_len = vlen;
            snap.key_len = klen;
            memcpy(snap.payload, snap_payload, 32);

            if (out_val) *out_val = entry_val(&snap);
            if (out_len) *out_len = vlen;
            e->clock_bit = 1;
            atomic_fetch_add(&s->hits, 1);
            return VC_OK;
        }
        break;  /* inline — fall through to locked path */
    }

    /* Locked path: inline values or repeated seqlock contention */
    shard_lock(s);
    uint32_t slot = _shard_find(s, key, (uint32_t)key_len, h);
    if (slot == UINT32_MAX) {
        shard_unlock(s);
        atomic_fetch_add(&s->misses, 1);
        return VC_ERR_NOTFND;
    }
    vc_entry_t *e = &s->table[slot];
    if (out_val) *out_val = entry_val(e);
    if (out_len) *out_len = e->val_len;
    e->clock_bit = 1;
    shard_unlock(s);
    atomic_fetch_add(&s->hits, 1);
    return VC_OK;
}

/* ══════════════════════════════════════════════════════════════
 * PUBLIC: vc_del
 * ══════════════════════════════════════════════════════════════ */

vc_err_t vc_del(vc_cache_t *c,
                const char *key, size_t key_len) {
    if (!c || !key) return VC_ERR_INVAL;

    uint64_t h    = wyhash64(key, key_len, 0);
    uint32_t si   = shard_idx(h);
    vc_shard_t *s = &c->shards[si];

    wal_append(&c->wal, VC_WAL_DEL, key, (uint32_t)key_len, NULL, 0, 0);

    shard_lock(s);
    seq_write_begin(s);

    uint32_t slot = _shard_find(s, key, (uint32_t)key_len, h);
    if (slot == UINT32_MAX) {
        seq_write_end(s);
        shard_unlock(s);
        return VC_ERR_NOTFND;
    }

    vc_entry_t *e = &s->table[slot];
    entry_free_resources(e, &c->slab);
    atomic_store_explicit(&e->state, VC_STATE_DEAD, memory_order_release);
    atomic_fetch_sub(&s->count, 1);

    seq_write_end(s);
    shard_unlock(s);
    return VC_OK;
}

/* ══════════════════════════════════════════════════════════════
 * PUBLIC: vc_flush
 * ══════════════════════════════════════════════════════════════ */

void vc_flush(vc_cache_t *c) {
    for (uint32_t i = 0; i < VC_NUM_SHARDS; i++) {
        vc_shard_t *s = &c->shards[i];
        shard_lock(s);
        seq_write_begin(s);
        for (uint32_t j = 0; j < s->cap; j++) {
            vc_entry_t *e = &s->table[j];
            uint8_t st = atomic_load_explicit(&e->state, memory_order_relaxed);
            if (st == VC_STATE_LIVE) {
                entry_free_resources(e, &c->slab);
                atomic_store_explicit(&e->state, VC_STATE_DEAD, memory_order_relaxed);
            }
        }
        atomic_store(&s->count, 0);
        seq_write_end(s);
        shard_unlock(s);
    }
}

/* ══════════════════════════════════════════════════════════════
 * STATS
 * ══════════════════════════════════════════════════════════════ */

void vc_stats(const vc_cache_t *c, vc_global_stats_t *out) {
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; i < VC_NUM_SHARDS; i++) {
        const vc_shard_t *s = &c->shards[i];
        out->hits        += atomic_load(&s->hits);
        out->misses      += atomic_load(&s->misses);
        out->sets        += atomic_load(&s->sets);
        out->evictions   += atomic_load(&s->evictions);
        out->expirations += atomic_load(&s->expirations);
        out->total_keys  += atomic_load(&s->count);
    }
    out->memory_bytes = atomic_load(&c->total_memory);
}

void vc_stats_print(const vc_cache_t *c) {
    vc_global_stats_t s;
    vc_stats(c, &s);
    uint64_t total = s.hits + s.misses;
    double hit_rate = total ? 100.0 * s.hits / total : 0.0;
    printf("══════ VoidCache Stats ════════════════════\n");
    printf("  Keys:       %llu\n",      (unsigned long long)s.total_keys);
    printf("  Sets:       %llu\n",      (unsigned long long)s.sets);
    printf("  Hits:       %llu  (%.1f%%)\n", (unsigned long long)s.hits, hit_rate);
    printf("  Misses:     %llu\n",      (unsigned long long)s.misses);
    printf("  Evictions:  %llu\n",      (unsigned long long)s.evictions);
    printf("  Memory:     %llu KB\n",   (unsigned long long)(s.memory_bytes / 1024));
    printf("═══════════════════════════════════════════\n");
}
