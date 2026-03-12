# build-windows.ps1  —  Build vcli.exe on Windows using MSYS2 UCRT64
#
# Run from PowerShell (as normal user, NOT Administrator):
#   Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
#   .\build-windows.ps1

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$MSYS2_DIR  = "C:\msys64"
$UCRT64_GCC = "$MSYS2_DIR\ucrt64\bin\gcc.exe"
$UCRT64_BIN = "$MSYS2_DIR\ucrt64\bin"
$PACMAN     = "$MSYS2_DIR\usr\bin\pacman.exe"
# Use MSYS2's own bash explicitly — NOT any other bash on PATH (e.g. Cygwin, Git)
$MSYS2_BASH = "$MSYS2_DIR\usr\bin\bash.exe"

Write-Host ""
Write-Host "=== VoidCache Windows Build ===" -ForegroundColor Cyan
Write-Host ""

# ── Step 1: Check MSYS2 ───────────────────────────────────────────────────────
if (-not (Test-Path $MSYS2_BASH)) {
    Write-Host "[1/4] MSYS2 not found at $MSYS2_DIR. Installing via winget..." -ForegroundColor Yellow
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if (-not $winget) {
        Write-Host "ERROR: winget not found. Install from https://aka.ms/getwinget" -ForegroundColor Red
        Write-Host "Or install MSYS2 manually from https://www.msys2.org/ to C:\msys64" -ForegroundColor Red
        exit 1
    }
    winget install --id MSYS2.MSYS2 --silent --accept-package-agreements --accept-source-agreements
    Start-Sleep -Seconds 5
    if (-not (Test-Path $MSYS2_BASH)) {
        Write-Host "ERROR: MSYS2 install failed or not at C:\msys64" -ForegroundColor Red
        exit 1
    }
}
Write-Host "[1/4] MSYS2 found at $MSYS2_DIR" -ForegroundColor Green

# ── Step 2: Install UCRT64 gcc + openssl via pacman ───────────────────────────
Write-Host "[2/4] Installing UCRT64 gcc and OpenSSL (wepoll+mman are bundled)..." -ForegroundColor Yellow

# Run pacman through MSYS2's OWN bash with --login to get the right environment
# CRITICAL: use $MSYS2_BASH explicitly, never rely on PATH-resolved bash
$packages = "mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-openssl make"
$pacmanCmd = "pacman -S --noconfirm --needed $packages"
& $MSYS2_BASH --login -c $pacmanCmd
if ($LASTEXITCODE -ne 0) {
    Write-Host "WARNING: pacman reported errors (may be harmless if packages already installed)" -ForegroundColor Yellow
}
Write-Host "[2/4] Packages ready." -ForegroundColor Green

# ── Step 3: Verify we have the RIGHT gcc (UCRT64, not Cygwin) ────────────────
if (-not (Test-Path $UCRT64_GCC)) {
    Write-Host "ERROR: UCRT64 gcc not found at $UCRT64_GCC" -ForegroundColor Red
    Write-Host "Run manually in MSYS2 UCRT64 terminal: pacman -S mingw-w64-ucrt-x86_64-gcc" -ForegroundColor Red
    exit 1
}
$gccVersion = & $UCRT64_GCC --version 2>&1 | Select-Object -First 1
Write-Host "[3/4] Compiler: $gccVersion" -ForegroundColor Green

# ── Step 4: Build directly with UCRT64 gcc — no bash, no make, no PATH issues ─
Write-Host "[4/4] Building vcli.exe..." -ForegroundColor Yellow

$SRC = Split-Path -Parent $MyInvocation.MyCommand.Path
$OPENSSL = "$MSYS2_DIR\ucrt64"

$Sources = @(
    "src\voidcache.c",
    "net\proto.c",
    "net\auth.c",
    "net\commands.c",
    "net\server.c",
    "net\cluster.c",
    "cli\vcli.c",
    "compat\wepoll.c",
    "compat\mman.c"
) | ForEach-Object { Join-Path $SRC $_ }

$Flags = @(
    "-O2", "-std=c11",
    "-Iinclude", "-Inet", "-Icompat",
    "-I$OPENSSL\include",
    "-D_WIN32_WINNT=0x0A00",
    "-DVCACHE_WINDOWS",
    "-Wno-unused-function", "-Wno-unused-parameter"
)

$Libs = @(
    "-L$OPENSSL\lib",
    "-lssl", "-lcrypto",
    "-lpthread",
    "-lws2_32", "-lbcrypt",
    "-lm"
)

$OutExe = Join-Path $SRC "vcli.exe"

$GccArgs = $Flags + $Sources + @("-o", $OutExe) + $Libs

Write-Host "  $UCRT64_GCC $($GccArgs -join ' ')" -ForegroundColor DarkGray
Write-Host ""

& $UCRT64_GCC @GccArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Build FAILED. See errors above." -ForegroundColor Red
    exit 1
}

# ── Copy required DLLs next to the binary ────────────────────────────────────
Write-Host ""
Write-Host "Copying runtime DLLs..." -ForegroundColor Yellow

$dlls = @(
    "libssl-3-x64.dll",
    "libcrypto-3-x64.dll",
    "libgcc_s_seh-1.dll",
    "libwinpthread-1.dll",
    "libstdc++-6.dll"
)
foreach ($dll in $dlls) {
    $src = Join-Path $UCRT64_BIN $dll
    if (Test-Path $src) {
        Copy-Item $src $SRC -Force
        Write-Host "  Copied $dll" -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host " Build complete!  vcli.exe is ready." -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "Test (cluster must be running):" -ForegroundColor White
Write-Host "  .\vcli.exe -h localhost -p 6379 PING" -ForegroundColor Cyan
Write-Host ""
Write-Host "Interactive shell:" -ForegroundColor White
Write-Host "  .\vcli.exe -h localhost -p 6379" -ForegroundColor Cyan
Write-Host ""
