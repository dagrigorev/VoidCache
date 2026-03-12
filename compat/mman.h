/*
 * compat/mman.h + mman.c — mmap/munmap for Windows
 *
 * Provides a POSIX mmap() compatible interface on top of
 * Windows VirtualAlloc / CreateFileMapping.
 *
 * Only the subset used by VoidCache is implemented:
 *   mmap(NULL, size, PROT_READ|PROT_WRITE,
 *        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)    → VirtualAlloc
 *   mmap(NULL, size, PROT_READ|PROT_WRITE,
 *        MAP_SHARED, fd, 0)                   → CreateFileMapping
 *   munmap(addr, size)                        → VirtualFree/UnmapViewOfFile
 *   msync(addr, size, flags)                  → FlushViewOfFile (no-op ok)
 */

#ifndef VCACHE_MMAN_H
#define VCACHE_MMAN_H

#ifdef _WIN32

#include <windows.h>
#include <sys/types.h>

/* mmap prot flags */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void*)-1)

/* msync flags */
#define MS_ASYNC      1
#define MS_SYNC       2
#define MS_INVALIDATE 4

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int   munmap(void* addr, size_t length);
int   msync(void* addr, size_t length, int flags);
int   mprotect(void* addr, size_t length, int prot);

#endif /* _WIN32 */
#endif /* VCACHE_MMAN_H */
