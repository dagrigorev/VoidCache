/*
 * compat/wepoll.h  —  Thin wrapper that routes to either real epoll (Linux/macOS)
 * or the wepoll library (Windows/MSYS2).
 *
 * On Windows (MSYS2/MinGW), install wepoll first:
 *   pacman -S mingw-w64-ucrt-x86_64-wepoll
 *
 * This header is included by net/server.c when _WIN32 is defined,
 * in place of <sys/epoll.h>.
 *
 * wepoll provides an identical API to Linux epoll:
 *   epoll_create1()  epoll_ctl()  epoll_wait()
 *   struct epoll_event  EPOLLIN  EPOLLOUT  EPOLLERR  EPOLLHUP  EPOLLET
 */

#ifndef VCACHE_COMPAT_WEPOLL_H
#define VCACHE_COMPAT_WEPOLL_H

#ifdef _WIN32
  /* wepoll installed via pacman -S mingw-w64-ucrt-x86_64-wepoll */
  #include <wepoll.h>
  /* wepoll uses HANDLE instead of int for epoll fds — typedef for compat */
  #ifndef EPOLL_FD_T
    #define EPOLL_FD_T HANDLE
  #endif
#else
  #include <sys/epoll.h>
  #ifndef EPOLL_FD_T
    #define EPOLL_FD_T int
  #endif
#endif

#endif /* VCACHE_COMPAT_WEPOLL_H */
