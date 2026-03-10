/*
 * wepoll.h — epoll for Windows
 *
 * Embedded from: https://github.com/piscisaureus/wepoll (MIT License)
 * This is the public API header. The implementation is in wepoll.c.
 *
 * Drop-in replacement for <sys/epoll.h> on Windows.
 * Compile wepoll.c alongside your project — no library needed.
 */

#ifndef WEPOLL_H_
#define WEPOLL_H_

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif

#include <stdint.h>
#include <windows.h>

/* Event types */
#define EPOLLIN      (1U <<  0)
#define EPOLLPRI     (1U <<  1)
#define EPOLLOUT     (1U <<  2)
#define EPOLLERR     (1U <<  3)
#define EPOLLHUP     (1U <<  4)
#define EPOLLRDNORM  (1U <<  6)
#define EPOLLRDBAND  (1U <<  7)
#define EPOLLWRNORM  (1U <<  8)
#define EPOLLWRBAND  (1U <<  9)
#define EPOLLMSG     (1U << 10)  /* never reported */
#define EPOLLRDHUP   (1U << 13)
#define EPOLLONESHOT (1U << 30)
#define EPOLLET      (1U << 31)

/* epoll_ctl() opcodes */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

/* epoll_create1() flags */
#define EPOLL_CLOEXEC 0x80000

typedef void*   HANDLE;

typedef union epoll_data {
    void*    ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
    SOCKET   sock;  /* Windows-specific */
    HANDLE   hnd;   /* Windows-specific */
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
};

#ifdef __cplusplus
extern "C" {
#endif

HANDLE epoll_create(int size);
HANDLE epoll_create1(int flags);
int    epoll_close(HANDLE ephnd);
int    epoll_ctl(HANDLE ephnd, int op, SOCKET sock, struct epoll_event* event);
int    epoll_wait(HANDLE ephnd, struct epoll_event* events, int maxevents, int timeout);

#ifdef __cplusplus
}
#endif

/* Map close() on an epoll HANDLE to epoll_close() */
#ifndef epoll_close
/* caller must use epoll_close() explicitly for epoll HANDLEs */
#endif

#endif /* _WIN32 */
#endif /* WEPOLL_H_ */
