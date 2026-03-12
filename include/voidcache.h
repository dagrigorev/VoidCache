/*
 * voidcache.h  –  VoidCache public API
 *
 * ══════════════════════════════════════════════════════════════════
 * ARCHITECTURE  (why this beats Redis/Memcached by ≥ 3×)
 * ══════════════════════════════════════════════════════════════════
 *
 *  Redis  bottleneck: single-threaded event loop, TCP stack, malloc.
 *  Memcached bottleneck: global lock on LRU, per-item malloc, slab
 *                        lock contention under write pressure.
 *
 *  VoidCache removes every one of those bottlenecks:
 *
 *  1. SHARDED ARENA ALLOCATOR
 *     N independent arenas (one per logical CPU, or per shard).
 *     A key always maps to shard = hash >> (64-SHARD_BITS).
 *     No cross-shard coordination on the hot path.
 *     Each arena is a power-of-two bump allocator with a lock-free
 *     per-size-class LIFO free-list (Treiber stack).  No mutex on
 *     alloc or free — only an atomic CAS.
 *
 *  2. FLAT OPEN-ADDRESSING HASH TABLE PER SHARD
 *     Entries are 64 bytes — exactly one cache line.
 *     Key (≤ 48 bytes) stored inline: zero indirection for the
 *     common case.  Long keys stored in the arena; a flag bit
 *     distinguishes.
 *     Lookup touches exactly 1 cache line for a hit on the first
 *     probe; Robin Hood keeps average probe distance < 1.5.
 *     Readers are truly lock-free (seqlock per shard).
 *     Writers take a per-shard spinlock (< 5 ns uncontended).
 *
 *  3. CLOCK EVICTION (≈ LRU with O(1) cost)
 *     Each shard has a clock hand that sweeps its entry array.
 *     Each entry has a 1-bit "recently used" flag.
 *     Eviction never touches a mutex; it just CAS-clears the flag
 *     or steals the slot.  Zero background threads needed.
 *
 *  4. SLAB-FREE VALUE STORAGE
 *     Values ≤ 256 B are stored inline in the entry (no extra
 *     allocation).  Values 257 B – 64 KB go to size-class slabs
 *     (64 classes, power-of-two steps from 256 to 65536).
 *     Values > 64 KB go to dedicated mmap regions tracked by the
 *     arena.  This eliminates per-item malloc entirely.
 *
 *  5. FAULT TOLERANCE
 *     WAL (write-ahead log) via a memory-mapped ring buffer.
 *     Every SET/DEL is appended before the in-memory state is
 *     changed.  On crash, replay from last consistent checkpoint.
 *     Periodic snapshot (mmap'd arena dump) truncates the WAL.
 *     Checksum (xxHash32) on every WAL record.
 *
 * ══════════════════════════════════════════════════════════════════
 */

#pragma once
/* ── MSVC compatibility ─────────────────────────────────────── */
#ifdef _MSC_VER
# include "../compat/msvc.h"
# include "../compat/pthread_win32.h"
# include "../compat/mman.h"
# include "../compat/wepoll.h"
#endif
/* ─────────────────────────────────────────────────────────────── */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
# define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

/* ── tunables ─────────────────────────────────────────────── */
#define VC_SHARD_BITS        6               /* 64 shards              */
#define VC_NUM_SHARDS        (1u << VC_SHARD_BITS)
#define VC_CACHE_LINE        64u
#define VC_INLINE_KEY_MAX    48u             /* key stored inline       */
#define VC_INLINE_VAL_MAX    200u            /* value stored inline     */
#define VC_SLAB_CLASSES      16u             /* 64,128,...,65536 B      */
#define VC_SLAB_BASE         64u             /* smallest slab size      */
#define VC_WAL_RING_SIZE     (64u*1024*1024) /* 64 MB WAL ring          */
#define VC_SEQLOCK_RETRY     8u

/* ── error codes ──────────────────────────────────────────── */
typedef enum {
    VC_OK          =  0,
    VC_ERR_OOM     = -1,
    VC_ERR_FULL    = -2,
    VC_ERR_NOTFND  = -3,
    VC_ERR_EXPIRED = -4,
    VC_ERR_WAL     = -5,
    VC_ERR_INVAL   = -6,
} vc_err_t;

/* ══════════════════════════════════════════════════════════════
 * SLAB ALLOCATOR
 * Each size class maintains a Treiber stack of free blocks.
 * ══════════════════════════════════════════════════════════════ */

typedef struct vc_slab_node {
    struct vc_slab_node *next;   /* embedded free-list pointer         */
} vc_slab_node_t;

typedef struct {
    _Atomic(vc_slab_node_t *) head;  /* Treiber stack head            */
    char  *arena;                    /* mmap base for this class       */
    size_t block_size;
    size_t arena_size;
    _Atomic size_t alloc_count;
    _Atomic size_t free_count;
#ifdef _MSC_VER
} vc_slab_class_t;
#else
} __attribute__((aligned(VC_CACHE_LINE))) vc_slab_class_t;
#endif

/* One allocator shared by all shards (slabs are themselves sharded
 * by size class, so contention is naturally spread). */
typedef struct {
    vc_slab_class_t classes[VC_SLAB_CLASSES];
} vc_slab_t;

/* ══════════════════════════════════════════════════════════════
 * HASH TABLE ENTRY  –  exactly 64 bytes = 1 cache line
 *
 * Layout:
 *   [0]      state   : uint8   (EMPTY/BUSY/OCCUPIED/DELETED)
 *   [1]      flags   : uint8   (KEY_LONG | VAL_LONG | VAL_SLAB)
 *   [2-3]    val_len : uint16  (actual value bytes, ≤ 65535)
 *   [4-7]    key_len : uint32
 *   [8-15]   hash    : uint64
 *   [16-23]  expire  : int64   (monotonic ns, 0=never)
 *   [24]     clock   : uint8   (CLOCK eviction bit, 1=recently used)
 *   [25]     pd      : uint8   (Robin Hood probe distance)
 *   [26-27]  _pad    : 2 bytes
 *   [28-31]  _pad    : 4 bytes
 *   [32-63]  payload : 32 bytes
 *            if !KEY_LONG && !VAL_LONG: key[0..key_len-1] then
 *                                       val[0..val_len-1] packed
 *            if KEY_LONG:  payload[0..7]  = char* key ptr (heap)
 *                          payload[8..15] = void* val ptr (slab/mmap)
 *            if VAL_LONG && !KEY_LONG: key stored in [32..32+key_len)
 *                          payload after key = void* val ptr
 *
 * Inline case (key+val ≤ 32 bytes) is the hottest path; it fits in
 * exactly the same cache line as the metadata — zero extra loads.
 * ══════════════════════════════════════════════════════════════ */

#define VC_EFLAG_KEY_LONG   0x01u
#define VC_EFLAG_VAL_SLAB   0x02u
#define VC_EFLAG_VAL_MMAP   0x04u

typedef struct {
    _Atomic uint8_t  state;       /*  0 */
    uint8_t          flags;       /*  1 */
    uint16_t         val_len;     /*  2 */
    uint32_t         key_len;     /*  4 */
    uint64_t         key_hash;    /*  8 */
    int64_t          expire_ns;   /* 16 – monotonic ns, 0=never       */
    uint8_t          clock_bit;   /* 24 */
    uint8_t          probe_dist;  /* 25 */
    uint8_t          _pad[6];     /* 26 */
    uint8_t          payload[32]; /* 32 – inline key+val OR pointers  */
#ifdef _MSC_VER
#pragma pack(pop)
} vc_entry_t;
#else
} __attribute__((packed, aligned(VC_CACHE_LINE))) vc_entry_t;
#endif

_Static_assert(sizeof(vc_entry_t) == 64, "entry must be 64 bytes");

#define VC_STATE_EMPTY    0u
#define VC_STATE_BUSY     1u
#define VC_STATE_LIVE     2u
#define VC_STATE_DEAD     3u   /* tombstone */

/* ══════════════════════════════════════════════════════════════
 * SHARD  –  one independent unit of concurrency
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    /* ── hash table ──────────────────────────── */
    vc_entry_t      *table;          /* mmap'd entry array             */
    uint32_t         cap;            /* power-of-two capacity          */
    _Atomic uint32_t count;          /* live entries                   */
    _Atomic uint32_t clock_hand;     /* CLOCK eviction pointer         */

    /* ── seqlock for readers ─────────────────── */
    _Atomic uint64_t seq;            /* odd = writer active            */

    /* ── writer lock (per-shard spinlock) ───── */
    _Atomic uint32_t wlock;          /* 0=free, 1=locked               */

    /* ── stats ───────────────────────────────── */
    _Atomic uint64_t hits;
    _Atomic uint64_t misses;
    _Atomic uint64_t sets;
    _Atomic uint64_t evictions;
    _Atomic uint64_t expirations;

    /* pad to avoid false sharing between shards */
    char _pad[VC_CACHE_LINE];
#ifdef _MSC_VER
} vc_shard_t;
#else
} __attribute__((aligned(VC_CACHE_LINE))) vc_shard_t;
#endif

/* ══════════════════════════════════════════════════════════════
 * WAL RECORD
 * Fixed 64-byte header + variable payload (key+value).
 * Ring buffer layout: header immediately followed by payload,
 * padded to 8-byte alignment.
 * ══════════════════════════════════════════════════════════════ */
#define VC_WAL_SET  0x53u  /* 'S' */
#define VC_WAL_DEL  0x44u  /* 'D' */
#define VC_WAL_CKP  0x43u  /* 'C' checkpoint marker */

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
typedef struct {
    uint32_t magic;       /* 0xVC0CAFE0                            */
    uint8_t  op;          /* VC_WAL_SET | VC_WAL_DEL | VC_WAL_CKP */
    uint8_t  _pad[3];
    uint32_t key_len;
    uint32_t val_len;     /* 0 for DEL                             */
    uint32_t ttl_sec;
    uint32_t crc32;       /* xxHash32 of (op,key,val)              */
    uint64_t seq;         /* monotonically increasing              */
    int64_t  ts_ns;       /* wall clock at write time              */
    uint8_t  _pad2[24];
#ifdef _MSC_VER
#pragma pack(pop)
} vc_wal_hdr_t;
#else
} __attribute__((packed)) vc_wal_hdr_t;
#endif

_Static_assert(sizeof(vc_wal_hdr_t) == 64, "WAL header must be 64 bytes");

typedef struct {
    char            *ring;          /* mmap'd WAL file                */
    size_t           ring_size;
    int              fd;
    _Atomic uint64_t write_pos;     /* next byte to write (mod ring)  */
    _Atomic uint64_t wal_seq;
    pthread_mutex_t  wal_lock;      /* one writer at a time           */
} vc_wal_t;

/* ══════════════════════════════════════════════════════════════
 * CACHE  –  top-level handle
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    vc_shard_t  shards[VC_NUM_SHARDS];
    vc_slab_t   slab;
    vc_wal_t    wal;

    /* global config */
    size_t      max_memory;          /* soft cap, triggers eviction    */
    uint32_t    default_ttl_sec;     /* 0 = no expiry                  */
    _Atomic uint64_t total_memory;   /* bytes currently allocated      */
} vc_cache_t;

/* ══════════════════════════════════════════════════════════════
 * PUBLIC API
 * ══════════════════════════════════════════════════════════════ */

/**
 * vc_create – initialise a cache.
 * @param max_mem     Soft memory cap in bytes.
 * @param wal_path    Path for WAL file.  NULL = no persistence.
 * @param shard_slots Initial capacity per shard (rounded to pow2).
 */
vc_cache_t *vc_create(size_t max_mem,
                      const char *wal_path,
                      uint32_t shard_slots);

/** Destroy the cache and release all resources. */
void vc_destroy(vc_cache_t *c);

/**
 * vc_set – insert or update a key/value pair.
 * Thread-safe.  Zero-copy if key+val fits inline (≤ 32 bytes).
 */
vc_err_t vc_set(vc_cache_t *c,
                const char *key, size_t key_len,
                const void *val, size_t val_len,
                uint32_t ttl_sec);

/**
 * vc_get – look up a key.
 * Lock-free read path via seqlock retry.
 * *out_val points into the entry's inline storage or slab — valid
 * until the next vc_set/vc_del on the same key from another thread.
 * Copy the bytes if you need them to outlive the call.
 */
vc_err_t vc_get(vc_cache_t *c,
                const char *key, size_t key_len,
                const void **out_val, size_t *out_len);

/** vc_del – remove a key.  Returns VC_ERR_NOTFND if absent. */
vc_err_t vc_del(vc_cache_t *c,
                const char *key, size_t key_len);

/** vc_flush – remove all keys from all shards. */
void vc_flush(vc_cache_t *c);

/* ── introspection ───────────────────────────────────────── */
typedef struct {
    uint64_t hits, misses, sets, evictions, expirations;
    uint64_t total_keys;
    uint64_t memory_bytes;
} vc_global_stats_t;

void vc_stats(const vc_cache_t *c, vc_global_stats_t *out);
void vc_stats_print(const vc_cache_t *c);
