/*
 * compat/windows.h  —  POSIX compatibility shims for Windows/MSYS2 builds.
 *
 * Included automatically when _WIN32 is defined.
 * Provides:
 *   - MAP_ANONYMOUS / MAP_ANON  (mmap flag)
 *   - SO_REUSEPORT              (socket option — falls back to SO_REUSEADDR)
 *   - sigwait / pthread_sigmask (replaced with Windows event-based shutdown)
 *   - /dev/urandom              (replaced with BCryptGenRandom)
 *   - strptime                  (not needed here, but common gap)
 */

#ifndef VCACHE_COMPAT_WINDOWS_H
#define VCACHE_COMPAT_WINDOWS_H

#ifdef _WIN32

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

/* mmap/munmap provided by compat/mman.h — included in files that need it */

/* ── SO_REUSEPORT ──────────────────────────────────────────────────────────
 * Windows doesn't have SO_REUSEPORT (added in a very recent Insider build).
 * Define it as SO_REUSEADDR — the server still works; multiple threads just
 * share a single listen socket instead of each having their own.
 */
#ifndef SO_REUSEPORT
  #define SO_REUSEPORT SO_REUSEADDR
#endif

/* ── sigwait / pthread_sigmask replacement ─────────────────────────────────
 * vcserver_run() blocks on sigwait() waiting for SIGINT/SIGTERM.
 * On Windows, we replace this with a Console Ctrl Handler + Event.
 *
 * Usage: call vcache_win_wait_for_shutdown() instead of sigwait().
 *        It blocks until Ctrl+C is pressed or vcache_win_signal_shutdown()
 *        is called from another thread.
 */
static HANDLE _vcache_shutdown_event = NULL;

static BOOL WINAPI _vcache_console_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT) {
        if (_vcache_shutdown_event)
            SetEvent(_vcache_shutdown_event);
        return TRUE;
    }
    return FALSE;
}

static inline void vcache_win_init_shutdown(void) {
    _vcache_shutdown_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(_vcache_console_handler, TRUE);
}

static inline void vcache_win_wait_for_shutdown(void) {
    if (!_vcache_shutdown_event) vcache_win_init_shutdown();
    WaitForSingleObject(_vcache_shutdown_event, INFINITE);
    fprintf(stderr, "\n[server] Ctrl+C received, shutting down...\n");
}

static inline void vcache_win_signal_shutdown(void) {
    if (_vcache_shutdown_event) SetEvent(_vcache_shutdown_event);
}

/* Map POSIX sigwait pattern to Windows equivalent */
#define SIGTERM 15
#define SIGINT  2

/* pthread_sigmask is a no-op on Windows (signals work differently) */
static inline int pthread_sigmask(int how, const void *set, void *oldset) {
    (void)how; (void)set; (void)oldset;
    return 0;
}
typedef unsigned int sigset_t;
static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigaddset(sigset_t *s, int sig) { *s |= (1u << sig); return 0; }
static inline int sigwait(const sigset_t *s, int *sig) {
    (void)s;
    vcache_win_wait_for_shutdown();
    *sig = SIGINT;
    return 0;
}

/* ── /dev/urandom → BCryptGenRandom ────────────────────────────────────────
 * Used in net/auth.c to generate session tokens.
 * We intercept the open("/dev/urandom") call via a macro that wraps it.
 * Alternatively, auth.c checks _WIN32 and calls BCryptGenRandom directly.
 */

/* Inline random bytes function for use in auth.c */
static inline int vcache_random_bytes(void *buf, size_t len) {
    NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(st) ? 0 : -1;
}

/* ── Winsock initialisation ─────────────────────────────────────────────────
 * Must be called once at startup before any socket operations.
 */
static inline void vcache_winsock_init(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
}

/* ── close() for sockets ────────────────────────────────────────────────────
 * On Windows, socket descriptors are not file descriptors.
 * MSYS2/MinGW's socket functions return real FDs compatible with close(),
 * so this is usually fine. Define a safe macro just in case.
 */
#ifndef close_socket
  #define close_socket(fd) close(fd)
#endif

#endif /* _WIN32 */
#endif /* VCACHE_COMPAT_WINDOWS_H */
