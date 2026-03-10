/*
 * compat/pthread_win32.h  —  pthreads over Win32 for MSVC
 *
 * Implements the pthread subset used by VoidCache:
 *   pthread_t / pthread_create / pthread_join
 *   pthread_mutex_t / pthread_mutex_lock / pthread_mutex_unlock / pthread_mutex_init / pthread_mutex_destroy
 *   pthread_rwlock_t / pthread_rwlock_rdlock / pthread_rwlock_wrlock / pthread_rwlock_unlock
 *   pthread_once_t / pthread_once
 *
 * Uses Win32 CRITICAL_SECTION for mutex and SRWLOCK for rwlock.
 * Uses _beginthreadex for thread creation (safer than CreateThread with CRT).
 */

#pragma once
#ifndef VCACHE_PTHREAD_WIN32_H
#define VCACHE_PTHREAD_WIN32_H

#ifdef _MSC_VER

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#include <errno.h>

/* ── pthread_t ───────────────────────────────────────────────────────────── */
typedef HANDLE pthread_t;
typedef void   pthread_attr_t;   /* unused — NULLs accepted */

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} _vc_thread_ctx;

static unsigned __stdcall _vc_thread_trampoline(void *ctx_) {
    _vc_thread_ctx *ctx = (_vc_thread_ctx*)ctx_;
    void *(*fn)(void*) = ctx->start_routine;
    void *arg          = ctx->arg;
    free(ctx);
    fn(arg);
    return 0;
}

static inline int pthread_create(pthread_t *t, const pthread_attr_t *attr,
                                  void *(*fn)(void*), void *arg) {
    (void)attr;
    _vc_thread_ctx *ctx = (_vc_thread_ctx*)malloc(sizeof(*ctx));
    if (!ctx) return ENOMEM;
    ctx->start_routine = fn;
    ctx->arg = arg;
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, _vc_thread_trampoline, ctx, 0, NULL);
    if (!h) { free(ctx); return EAGAIN; }
    *t = h;
    return 0;
}

static inline int pthread_join(pthread_t t, void **retval) {
    (void)retval;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}

static inline pthread_t pthread_self(void) {
    return GetCurrentThread();
}

/* ── pthread_mutex_t  ────────────────────────────────────────────────────── */
typedef CRITICAL_SECTION pthread_mutex_t;
typedef void             pthread_mutexattr_t;

static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
    (void)a;
    InitializeCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    DeleteCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    EnterCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    LeaveCriticalSection(m);
    return 0;
}
static inline int pthread_mutex_trylock(pthread_mutex_t *m) {
    return TryEnterCriticalSection(m) ? 0 : EBUSY;
}

/* Static mutex initialiser — not standard but used in some configs */
#define PTHREAD_MUTEX_INITIALIZER  {0}   /* zero-init is valid for CRITICAL_SECTION */

/* ── pthread_rwlock_t  ───────────────────────────────────────────────────── */
/* Wrap SRWLOCK with a flag so pthread_rwlock_unlock() knows which to release */
typedef struct {
    SRWLOCK lock;
    volatile LONG exclusive;  /* 1 = held exclusive, 0 = held shared */
} pthread_rwlock_t;
typedef void pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER {{0}, 0}

static inline int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *a) {
    (void)a;
    InitializeSRWLock(&rw->lock);
    rw->exclusive = 0;
    return 0;
}
static inline int pthread_rwlock_destroy(pthread_rwlock_t *rw) {
    (void)rw; return 0;
}
static inline int pthread_rwlock_rdlock(pthread_rwlock_t *rw) {
    AcquireSRWLockShared(&rw->lock);
    rw->exclusive = 0;
    return 0;
}
static inline int pthread_rwlock_wrlock(pthread_rwlock_t *rw) {
    AcquireSRWLockExclusive(&rw->lock);
    rw->exclusive = 1;
    return 0;
}
static inline int pthread_rwlock_unlock(pthread_rwlock_t *rw) {
    if (rw->exclusive) {
        rw->exclusive = 0;
        ReleaseSRWLockExclusive(&rw->lock);
    } else {
        ReleaseSRWLockShared(&rw->lock);
    }
    return 0;
}

/* ── pthread_once_t ──────────────────────────────────────────────────────── */
typedef INIT_ONCE pthread_once_t;
#define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT

static BOOL CALLBACK _vc_once_callback(INIT_ONCE *o, void *param, void **ctx) {
    (void)o; (void)ctx;
    ((void(*)(void))param)();
    return TRUE;
}
static inline int pthread_once(pthread_once_t *o, void (*fn)(void)) {
    InitOnceExecuteOnce(o, _vc_once_callback, (void*)fn, NULL);
    return 0;
}

#endif /* _MSC_VER */
#endif /* VCACHE_PTHREAD_WIN32_H */
