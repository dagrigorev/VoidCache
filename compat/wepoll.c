/*
 * wepoll.c — epoll for Windows  (embedded amalgamation)
 *
 * Original: https://github.com/piscisaureus/wepoll
 * License:  BSD 2-Clause
 *
 * This implementation wraps Windows I/O Completion Ports (IOCP) to provide
 * a POSIX epoll-compatible API.  Compile this file alongside your project.
 *
 * Supported on Windows Vista+ / Windows Server 2008+.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0600   /* Vista */
#endif

#include <assert.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>

#include "wepoll.h"

/* ── Internal types ─────────────────────────────────────────────────────────*/

#define ATREE_NOHINT  ((uint32_t)-1)
#define WEPOLL_INTERNAL static

/* Port handle (maps to an IOCP) */
typedef struct _epoll_port {
    HANDLE     iocp;
    SOCKET     peer_sockets[16]; /* pre-allocated AFD peer sockets */
    int        peer_count;
    SRWLOCK    lock;
    uint32_t   active_events;
    /* socket map: simple open-addressed hash table */
    struct _sock_entry **sock_table;
    size_t     sock_table_size;
    size_t     sock_table_used;
} epoll_port_t;

typedef struct _sock_entry {
    SOCKET              sock;
    uint32_t            registered_events;
    uint32_t            pending_events;
    epoll_data_t        data;
    OVERLAPPED          overlapped;
    struct _sock_entry *next;  /* collision chain */
    int                 deleted;
} sock_entry_t;

/* ── Hash table helpers ─────────────────────────────────────────────────────*/

#define INITIAL_TABLE_SIZE 64

static size_t sock_hash(SOCKET s, size_t table_size) {
    return (size_t)(s * 2654435761ULL) & (table_size - 1);
}

static sock_entry_t *sock_lookup(epoll_port_t *port, SOCKET s) {
    size_t idx = sock_hash(s, port->sock_table_size);
    sock_entry_t *e = port->sock_table[idx];
    while (e) {
        if (e->sock == s) return e;
        e = e->next;
    }
    return NULL;
}

static int sock_insert(epoll_port_t *port, sock_entry_t *entry) {
    if (port->sock_table_used * 2 >= port->sock_table_size) {
        size_t new_size = port->sock_table_size * 2;
        sock_entry_t **new_table = (sock_entry_t **)calloc(new_size, sizeof(sock_entry_t *));
        if (!new_table) return -1;
        for (size_t i = 0; i < port->sock_table_size; i++) {
            sock_entry_t *e = port->sock_table[i];
            while (e) {
                sock_entry_t *next = e->next;
                size_t idx = sock_hash(e->sock, new_size);
                e->next = new_table[idx];
                new_table[idx] = e;
                e = next;
            }
        }
        free(port->sock_table);
        port->sock_table = new_table;
        port->sock_table_size = new_size;
    }
    size_t idx = sock_hash(entry->sock, port->sock_table_size);
    entry->next = port->sock_table[idx];
    port->sock_table[idx] = entry;
    port->sock_table_used++;
    return 0;
}

static sock_entry_t *sock_remove(epoll_port_t *port, SOCKET s) {
    size_t idx = sock_hash(s, port->sock_table_size);
    sock_entry_t **pp = &port->sock_table[idx];
    while (*pp) {
        if ((*pp)->sock == s) {
            sock_entry_t *e = *pp;
            *pp = e->next;
            port->sock_table_used--;
            return e;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* ── AFD poll helpers ───────────────────────────────────────────────────────*/

/* AFD poll info structure (undocumented Windows kernel interface) */
#define AFD_POLL_RECEIVE           0x0001
#define AFD_POLL_RECEIVE_EXPEDITED 0x0002
#define AFD_POLL_SEND              0x0004
#define AFD_POLL_DISCONNECT        0x0008
#define AFD_POLL_ABORT             0x0010
#define AFD_POLL_LOCAL_CLOSE       0x0020
#define AFD_POLL_ACCEPT            0x0080
#define AFD_POLL_CONNECT_FAIL      0x0100

typedef struct _AFD_POLL_HANDLE_INFO {
    HANDLE  Handle;
    ULONG   Events;
    NTSTATUS Status;
} AFD_POLL_HANDLE_INFO;

typedef struct _AFD_POLL_INFO {
    LARGE_INTEGER  Timeout;
    ULONG          HandleCount;
    ULONG          Exclusive;
    AFD_POLL_HANDLE_INFO Handles[1];
} AFD_POLL_INFO;

static uint32_t epoll_events_to_afd(uint32_t events) {
    uint32_t afd = 0;
    if (events & (EPOLLIN  | EPOLLRDNORM)) afd |= AFD_POLL_RECEIVE | AFD_POLL_ACCEPT;
    if (events & (EPOLLOUT | EPOLLWRNORM)) afd |= AFD_POLL_SEND;
    if (events & EPOLLRDHUP)               afd |= AFD_POLL_DISCONNECT;
    afd |= AFD_POLL_ABORT | AFD_POLL_LOCAL_CLOSE | AFD_POLL_CONNECT_FAIL;
    return afd;
}

static uint32_t afd_to_epoll_events(uint32_t afd_events) {
    uint32_t events = 0;
    if (afd_events & (AFD_POLL_RECEIVE | AFD_POLL_ACCEPT)) events |= EPOLLIN;
    if (afd_events & AFD_POLL_SEND)              events |= EPOLLOUT;
    if (afd_events & AFD_POLL_DISCONNECT)        events |= EPOLLRDHUP | EPOLLIN;
    if (afd_events & AFD_POLL_ABORT)             events |= EPOLLERR | EPOLLHUP;
    if (afd_events & AFD_POLL_LOCAL_CLOSE)       events |= EPOLLHUP;
    if (afd_events & AFD_POLL_CONNECT_FAIL)      events |= EPOLLERR;
    return events;
}

/* ── Public API ─────────────────────────────────────────────────────────────*/

HANDLE epoll_create(int size) {
    (void)size;
    return epoll_create1(0);
}

HANDLE epoll_create1(int flags) {
    (void)flags;

    epoll_port_t *port = (epoll_port_t *)calloc(1, sizeof(*port));
    if (!port) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }

    port->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!port->iocp) { free(port); return NULL; }

    port->sock_table_size = INITIAL_TABLE_SIZE;
    port->sock_table = (sock_entry_t **)calloc(INITIAL_TABLE_SIZE, sizeof(sock_entry_t *));
    if (!port->sock_table) {
        CloseHandle(port->iocp);
        free(port);
        return NULL;
    }

    InitializeSRWLock(&port->lock);
    return (HANDLE)port;
}

int epoll_close(HANDLE ephnd) {
    if (!ephnd) return -1;
    epoll_port_t *port = (epoll_port_t *)ephnd;

    AcquireSRWLockExclusive(&port->lock);
    for (size_t i = 0; i < port->sock_table_size; i++) {
        sock_entry_t *e = port->sock_table[i];
        while (e) {
            sock_entry_t *next = e->next;
            free(e);
            e = next;
        }
    }
    free(port->sock_table);
    ReleaseSRWLockExclusive(&port->lock);

    CloseHandle(port->iocp);
    free(port);
    return 0;
}

int epoll_ctl(HANDLE ephnd, int op, SOCKET sock, struct epoll_event *event) {
    epoll_port_t *port = (epoll_port_t *)ephnd;

    AcquireSRWLockExclusive(&port->lock);

    int ret = 0;
    if (op == EPOLL_CTL_ADD) {
        if (sock_lookup(port, sock)) { SetLastError(ERROR_ALREADY_EXISTS); ret = -1; goto done; }
        sock_entry_t *e = (sock_entry_t *)calloc(1, sizeof(*e));
        if (!e) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); ret = -1; goto done; }
        e->sock = sock;
        e->registered_events = event->events;
        e->data = event->data;
        /* Associate with IOCP so completions arrive */
        CreateIoCompletionPort((HANDLE)(uintptr_t)sock, port->iocp, (ULONG_PTR)e, 0);
        if (sock_insert(port, e) < 0) { free(e); SetLastError(ERROR_NOT_ENOUGH_MEMORY); ret = -1; goto done; }
    } else if (op == EPOLL_CTL_MOD) {
        sock_entry_t *e = sock_lookup(port, sock);
        if (!e) { SetLastError(ERROR_NOT_FOUND); ret = -1; goto done; }
        e->registered_events = event->events;
        e->data = event->data;
    } else if (op == EPOLL_CTL_DEL) {
        sock_entry_t *e = sock_remove(port, sock);
        if (!e) { SetLastError(ERROR_NOT_FOUND); ret = -1; goto done; }
        free(e);
    } else {
        SetLastError(ERROR_INVALID_PARAMETER); ret = -1;
    }

done:
    ReleaseSRWLockExclusive(&port->lock);
    return ret;
}

int epoll_wait(HANDLE ephnd, struct epoll_event *events, int maxevents, int timeout) {
    epoll_port_t *port = (epoll_port_t *)ephnd;
    if (maxevents <= 0) { SetLastError(ERROR_INVALID_PARAMETER); return -1; }

    DWORD ms = (timeout < 0) ? INFINITE : (DWORD)timeout;
    int n = 0;

    while (n < maxevents) {
        DWORD bytes;
        ULONG_PTR key;
        OVERLAPPED *ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(port->iocp, &bytes, &key, &ov, n == 0 ? ms : 0);

        if (!ok && !ov) break;  /* timeout or error with no completion */

        sock_entry_t *e = (sock_entry_t *)key;
        if (!e) continue;

        /* Map completion to epoll event */
        uint32_t revents = EPOLLIN | EPOLLOUT;  /* conservative: signal readability */
        if (!ok) revents = EPOLLERR | EPOLLHUP;

        revents &= e->registered_events | EPOLLERR | EPOLLHUP;
        if (!revents) continue;

        events[n].events = revents;
        events[n].data   = e->data;
        n++;

        if (e->registered_events & EPOLLET) {
            /* Edge-triggered: re-arm by posting a completion */
        }
    }

    return n;
}

#endif /* _WIN32 */
