/*
 * compat/msvc.h  —  MSVC compatibility shims for VoidCache
 *
 * Included automatically when _MSC_VER is defined (Visual Studio 2019+).
 * Bridges every POSIX / GCC-specific API used by the codebase:
 *
 *   __attribute__((aligned / packed))  → __declspec(align) / #pragma pack
 *   _Atomic / stdatomic.h              → <stdatomic.h>  (VS2019+ ships it)
 *   ssize_t                            → typedef'd from SSIZE_T
 *   unistd.h (read/write/close/sleep)  → Winsock2 + io.h equivalents
 *   fcntl (F_GETFL/F_SETFL/O_NONBLOCK) → ioctlsocket()
 *   clock_gettime(CLOCK_MONOTONIC)     → QueryPerformanceCounter
 *   strdup                             → _strdup
 *   pthreads                           → compat/pthread_win32.h
 *   mmap / munmap                      → compat/mman.h
 *   epoll                              → compat/wepoll.h
 *   /dev/urandom                       → BCryptGenRandom
 *   SO_REUSEPORT                       → SO_REUSEADDR
 *   sigwait / Ctrl+C                   → SetConsoleCtrlHandler
 */

#pragma once
#ifndef VCACHE_MSVC_H
#define VCACHE_MSVC_H

#ifdef _MSC_VER

/* ── Windows headers ─────────────────────────────────────────────────────── */
#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0A00   /* Windows 10 */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

/* ── __attribute__ → MSVC equivalents ───────────────────────────────────── */
#define __attribute__(x)          /* strip GCC attributes */
/* Individual replacements used in voidcache.h: */
#define VC_ALIGNED(n)             __declspec(align(n))
/* packed: handled per-struct with #pragma pack in voidcache.h via MSVC_PACK */

/* ── ssize_t ─────────────────────────────────────────────────────────────── */
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;

/* ── Standard C99/POSIX types that MSVC may lack ─────────────────────────── */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── unistd.h equivalents ────────────────────────────────────────────────── */
/* read/write on sockets — use recv/send instead of read/write */
static inline ssize_t _vc_read(int fd, void *buf, size_t len) {
    return recv((SOCKET)fd, (char*)buf, (int)len, 0);
}
static inline ssize_t _vc_write(int fd, const void *buf, size_t len) {
    return send((SOCKET)fd, (const char*)buf, (int)len, 0);
}
static inline int _vc_close(int fd) {
    return closesocket((SOCKET)fd);
}
#define read(fd, buf, n)   _vc_read((fd), (buf), (n))
#define write(fd, buf, n)  _vc_write((fd), (buf), (n))
#define close(fd)          _vc_close(fd)
#define sleep(s)           Sleep((s) * 1000)
#define usleep(us)         Sleep((us) / 1000)
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

static inline int _vc_isatty(int fd) {
    HANDLE h = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE :
                            fd == 2 ? STD_ERROR_HANDLE  : STD_INPUT_HANDLE);
    DWORD mode;
    return GetConsoleMode(h, &mode) ? 1 : 0;
}
#define isatty(fd) _vc_isatty(fd)

/* ── fcntl → ioctlsocket ─────────────────────────────────────────────────── */
#define F_GETFL  3
#define F_SETFL  4
#define O_NONBLOCK 0x4000

static inline int _vc_fcntl(int fd, int cmd, int arg) {
    if (cmd == F_SETFL && (arg & O_NONBLOCK)) {
        u_long mode = 1;
        return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
    }
    if (cmd == F_SETFL && !(arg & O_NONBLOCK)) {
        u_long mode = 0;
        return ioctlsocket((SOCKET)fd, FIONBIO, &mode) == 0 ? 0 : -1;
    }
    if (cmd == F_GETFL) return 0;  /* can't query nonblocking state portably */
    return -1;
}
#define fcntl(fd, cmd, ...) _vc_fcntl((fd), (cmd), ##__VA_ARGS__ + 0)

/* ── O_RDONLY / O_WRONLY / O_CREAT etc ───────────────────────────────────── */
#include <fcntl.h>   /* MSVC's fcntl.h provides O_RDONLY etc for _open() */

/* ── clock_gettime ───────────────────────────────────────────────────────── */
#include <time.h>
#ifndef CLOCK_MONOTONIC
# define CLOCK_MONOTONIC 1
# define CLOCK_REALTIME  0

static inline int clock_gettime(int clk, struct timespec *ts) {
    if (clk == CLOCK_MONOTONIC) {
        LARGE_INTEGER freq, count;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&count);
        ts->tv_sec  = (time_t)(count.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)(((count.QuadPart % freq.QuadPart) * 1000000000LL)
                             / freq.QuadPart);
    } else {
        /* CLOCK_REALTIME */
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        t -= 116444736000000000ULL;  /* epoch diff: 1601→1970 in 100ns units */
        ts->tv_sec  = (time_t)(t / 10000000ULL);
        ts->tv_nsec = (long)((t % 10000000ULL) * 100);
    }
    return 0;
}
#endif /* CLOCK_MONOTONIC */

/* ── strdup ──────────────────────────────────────────────────────────────── */
#define strdup(s)  _strdup(s)

/* ── strtok_r → strtok_s ─────────────────────────────────────────────────── */
#define strtok_r(str, delim, save) strtok_s((str), (delim), (save))

/* ── snprintf: MSVC 2015+ has it; just ensure it's declared ─────────────── */
#include <stdio.h>

/* ── SO_REUSEPORT → SO_REUSEADDR ─────────────────────────────────────────── */
#ifndef SO_REUSEPORT
# define SO_REUSEPORT SO_REUSEADDR
#endif

#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

/* ── Signal/shutdown (Ctrl+C via SetConsoleCtrlHandler) ─────────────────── */
#define SIGTERM 15
#define SIGINT  2
typedef unsigned int sigset_t;
static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigaddset(sigset_t *s, int sig) { *s |= (1u << sig); return 0; }

static HANDLE _vcache_shutdown_event_msvc = NULL;
static BOOL WINAPI _vcache_ctrl_handler(DWORD t) {
    if (t == CTRL_C_EVENT || t == CTRL_BREAK_EVENT || t == CTRL_CLOSE_EVENT) {
        if (_vcache_shutdown_event_msvc) SetEvent(_vcache_shutdown_event_msvc);
        return TRUE;
    }
    return FALSE;
}
static inline int pthread_sigmask(int h, const sigset_t *s, sigset_t *o) {
    (void)h; (void)s; (void)o; return 0;
}
static inline int sigwait(const sigset_t *s, int *sig) {
    (void)s;
    if (!_vcache_shutdown_event_msvc) {
        _vcache_shutdown_event_msvc = CreateEventA(NULL, TRUE, FALSE, NULL);
        SetConsoleCtrlHandler(_vcache_ctrl_handler, TRUE);
    }
    WaitForSingleObject(_vcache_shutdown_event_msvc, INFINITE);
    *sig = SIGINT;
    return 0;
}

/* ── /dev/urandom → BCryptGenRandom ─────────────────────────────────────── */
static inline int vcache_random_bytes(void *buf, size_t len) {
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                                         BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ? 0 : -1;
}

/* ── Winsock init (call once at startup) ─────────────────────────────────── */
static inline void vcache_winsock_init(void) {
    static int done = 0;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2,2), &w); done = 1; }
}

/* ── errno mapping from WSA errors ──────────────────────────────────────── */
#include <errno.h>
static inline int _vc_socket_errno(void) {
    int e = WSAGetLastError();
    switch (e) {
        case WSAEWOULDBLOCK:  return EAGAIN;
        case WSAEINPROGRESS:  return EINPROGRESS;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAETIMEDOUT:    return ETIMEDOUT;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        default: return EIO;
    }
}
/* After any Winsock call that fails, use this to get POSIX errno */
#define vc_net_errno() _vc_socket_errno()

/* ── getaddrinfo / freeaddrinfo: available via ws2tcpip.h ────────────────── */
/* Already included above */

/* ── MSVC: disable common warnings that are noise for ported C code ──────── */
#pragma warning(disable: 4996)  /* 'deprecated' POSIX names */
#pragma warning(disable: 4200)  /* zero-length array in struct */
#pragma warning(disable: 4204)  /* non-constant aggregate initializer */
#pragma warning(disable: 4221)  /* initialisation using address of local var */

#endif /* _MSC_VER */
#endif /* VCACHE_MSVC_H */
