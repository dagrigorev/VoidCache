# VoidCache — Visual Studio 2022 Build Guide

## Prerequisites

| Tool | Where to get | Required? |
|---|---|---|
| VS2022 with **"Desktop development with C++"** | [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/) | ✅ Yes |
| OpenSSL for Windows (x64) | via vcpkg (easiest) or manual | ✅ Yes |
| Git | [git-scm.com](https://git-scm.com) | Only if using vcpkg |

---

## Quick Start (automated)

Open **PowerShell** in this `vs2022\` folder:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned   # once only
.\setup.ps1
```

The script installs OpenSSL via vcpkg and builds `vcli.exe` automatically.  
Output: `vs2022\x64\Release\vcli.exe`

---

## Manual Setup

### Step 1 — Install OpenSSL

**Option A: vcpkg (recommended)**
```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg --depth=1
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\vcpkg\vcpkg.exe install openssl:x64-windows
C:\vcpkg\vcpkg.exe integrate install
```

**Option B: Pre-built installer**
1. Download the Win64 OpenSSL installer from [slproweb.com/products/Win32OpenSSL.html](https://slproweb.com/products/Win32OpenSSL.html)
2. Install to `C:\Program Files\OpenSSL-Win64` (the default)
3. The project finds it automatically at that path

### Step 2 — Open and Build

```
File → Open → Project/Solution → vcli.sln
```

Select **Release | x64** in the toolbar, then **Build → Build Solution** (Ctrl+Shift+B).

Or from the **Developer Command Prompt for VS2022**:
```bat
cd vs2022
msbuild vcli.sln /p:Configuration=Release /p:Platform=x64 /m
```

### Step 3 — Run

```powershell
.\x64\Release\vcli.exe -h localhost -p 6379 PING

# Interactive shell
.\x64\Release\vcli.exe -h localhost -p 6379

# Run as server
.\x64\Release\vcli.exe server --port 6379
```

---

## Project Structure

```
VoidCache/
├── vs2022/
│   ├── vcli.sln          ← Open this in VS2022
│   ├── vcli.vcxproj      ← Project file (all settings here)
│   ├── setup.ps1         ← Automated first-time setup
│   └── README.md         ← This file
│
├── src/voidcache.c       ← Core cache engine (sharded HT, WAL, slab alloc)
├── net/                  ← Network layer (RESP3, epoll, TLS, cluster)
├── cli/vcli.c            ← CLI + server entry point
├── include/voidcache.h   ← Public API
│
└── compat/               ← Windows/MSVC compatibility shims
    ├── msvc.h            ← Master shim (ssize_t, fcntl, clock_gettime, etc.)
    ├── pthread_win32.h   ← pthreads → Win32 CRITICAL_SECTION / SRWLOCK
    ├── wepoll.h + .c     ← epoll → Windows IOCP
    └── mman.h + .c       ← mmap/munmap → VirtualAlloc / MapViewOfFile
```

---

## What the MSVC shims do

The codebase was written for Linux/GCC. Four layers of shims make it compile under MSVC:

### `compat/msvc.h` — POSIX API bridges
| POSIX | MSVC replacement |
|---|---|
| `ssize_t` | `typedef SSIZE_T ssize_t` |
| `unistd.h` (`read`/`write`/`close`) | `recv`/`send`/`closesocket` |
| `fcntl(F_SETFL, O_NONBLOCK)` | `ioctlsocket(FIONBIO)` |
| `clock_gettime(CLOCK_MONOTONIC)` | `QueryPerformanceCounter` |
| `strdup` | `_strdup` |
| `sigwait` / Ctrl+C | `SetConsoleCtrlHandler` |
| `/dev/urandom` | `BCryptGenRandom` |
| `SO_REUSEPORT` | `SO_REUSEADDR` |

### `compat/pthread_win32.h` — pthreads → Win32
| pthread | Win32 |
|---|---|
| `pthread_t` + `pthread_create/join` | `HANDLE` + `_beginthreadex` |
| `pthread_mutex_t` | `CRITICAL_SECTION` |
| `pthread_rwlock_t` | `SRWLOCK` (with exclusive-tracking wrapper) |
| `pthread_once_t` | `INIT_ONCE` |

### `compat/wepoll.h/.c` — epoll → IOCP
Drop-in `epoll_create/ctl/wait` using Windows I/O Completion Ports.

### `compat/mman.h/.c` — mmap → VirtualAlloc
- `MAP_ANONYMOUS` → `VirtualAlloc(MEM_COMMIT | MEM_RESERVE)`
- `MAP_SHARED` + file fd → `CreateFileMapping` + `MapViewOfFile`
- `munmap` → `UnmapViewOfFile` or `VirtualFree`

### `voidcache.h` — struct attributes
```c
// GCC:
} __attribute__((packed, aligned(64))) vc_entry_t;

// MSVC (via #ifdef guards in the header):
#pragma pack(push, 1)
typedef struct { ... } vc_entry_t;
#pragma pack(pop)
```

---

## Troubleshooting

**`Cannot open include file: 'openssl/ssl.h'`**
→ OpenSSL path not found. Set it in `vcli.vcxproj` under `<OpenSSLRoot>`, or run `setup.ps1`.

**`LNK1181: cannot open input file 'libssl.lib'`**
→ OpenSSL lib path wrong. Check that `$(OpenSSLRoot)\lib\libssl.lib` exists.
   For vcpkg installs: lib is at `C:\vcpkg\installed\x64-windows\lib\libssl.lib`

**`LNK2019: unresolved external symbol BCryptGenRandom`**
→ Add `bcrypt.lib` to Additional Dependencies (already in the project, check it's there).

**Application crashes on startup**
→ OpenSSL DLLs not found at runtime. The post-build event copies them; if missing, copy manually:
   `libssl-3-x64.dll` and `libcrypto-3-x64.dll` from `$(OpenSSLRoot)\bin\` to the `.exe` folder.

**`C4024: different types for formal and actual parameter`** (wepoll)
→ wepoll uses `HANDLE` for the epoll fd; ensure `compat/wepoll.h` is included before `server.h`.

---

## Debug Build

Select **Debug | x64** in VS2022. The debug build:
- Links against `/MTd` (static debug CRT)  
- Disables optimisation (`/Od`)  
- Generates a `.pdb` for full debugger symbols  

Set a breakpoint anywhere in `vcli.c` or `voidcache.c` and press F5 to start under the debugger.
You can inspect cache shards, WAL state, and connection objects directly in the Watch window.
