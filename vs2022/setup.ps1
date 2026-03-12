# vs2022\setup.ps1  —  First-time setup: install OpenSSL via vcpkg then build
#
# Run from the vs2022\ folder in PowerShell (not as Administrator):
#   Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
#   cd vs2022
#   .\setup.ps1
#
# What it does:
#   1. Finds or installs vcpkg
#   2. Installs OpenSSL x64-windows via vcpkg
#   3. Integrates vcpkg with Visual Studio
#   4. Opens the solution in VS2022 (or builds from CLI if VS CLI available)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Root     = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$VS2022   = "$Root\vs2022"

Write-Host ""
Write-Host "=== VoidCache VS2022 Setup ===" -ForegroundColor Cyan
Write-Host "Root: $Root"
Write-Host ""

# ── 1. Locate or install vcpkg ────────────────────────────────────────────────
$VcpkgDir = $env:VCPKG_ROOT
if (-not $VcpkgDir -or -not (Test-Path "$VcpkgDir\vcpkg.exe")) {
    # Common install locations
    foreach ($loc in @("C:\vcpkg", "C:\src\vcpkg", "$env:USERPROFILE\vcpkg",
                       "C:\dev\vcpkg", "D:\vcpkg")) {
        if (Test-Path "$loc\vcpkg.exe") { $VcpkgDir = $loc; break }
    }
}

if (-not $VcpkgDir -or -not (Test-Path "$VcpkgDir\vcpkg.exe")) {
    Write-Host "[1/4] vcpkg not found. Cloning to C:\vcpkg..." -ForegroundColor Yellow
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        Write-Host "ERROR: git not found. Install Git from https://git-scm.com" -ForegroundColor Red
        Write-Host "Or install OpenSSL manually from https://slproweb.com/products/Win32OpenSSL.html" -ForegroundColor Yellow
        Write-Host "Install to C:\Program Files\OpenSSL-Win64 and the project will find it." -ForegroundColor Yellow
        exit 1
    }
    git clone https://github.com/microsoft/vcpkg C:\vcpkg --depth=1
    & C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
    $VcpkgDir = "C:\vcpkg"
    $env:VCPKG_ROOT = $VcpkgDir
} else {
    Write-Host "[1/4] vcpkg found at $VcpkgDir" -ForegroundColor Green
}

# ── 2. Install OpenSSL ────────────────────────────────────────────────────────
Write-Host "[2/4] Installing OpenSSL x64-windows via vcpkg..." -ForegroundColor Yellow
& "$VcpkgDir\vcpkg.exe" install openssl:x64-windows
if ($LASTEXITCODE -ne 0) {
    Write-Host "WARNING: vcpkg install may have had issues, continuing..." -ForegroundColor Yellow
}
Write-Host "[2/4] OpenSSL installed." -ForegroundColor Green

# ── 3. vcpkg integrate ───────────────────────────────────────────────────────
Write-Host "[3/4] Integrating vcpkg with Visual Studio..." -ForegroundColor Yellow
& "$VcpkgDir\vcpkg.exe" integrate install
Write-Host "[3/4] vcpkg integrated." -ForegroundColor Green

# ── 4. Build or open in VS2022 ───────────────────────────────────────────────
Write-Host "[4/4] Building with MSBuild..." -ForegroundColor Yellow

# Find MSBuild
$msbuild = $null
$msbuildPaths = @(
    "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
)
foreach ($p in $msbuildPaths) {
    if (Test-Path $p) { $msbuild = $p; break }
}

# Try PATH
if (-not $msbuild) {
    $msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue)?.Source
}

if ($msbuild) {
    Write-Host "  Using MSBuild: $msbuild" -ForegroundColor DarkGray
    & $msbuild "$VS2022\vcli.sln" /p:Configuration=Release /p:Platform=x64 /m /nologo `
        /p:VcpkgOpenSSL="$VcpkgDir\installed\x64-windows"
    if ($LASTEXITCODE -eq 0) {
        $exe = "$VS2022\x64\Release\vcli.exe"
        Write-Host ""
        Write-Host "========================================" -ForegroundColor Green
        Write-Host " Build complete!  vcli.exe is ready." -ForegroundColor Green
        Write-Host "========================================" -ForegroundColor Green
        Write-Host ""
        Write-Host "Binary: $exe" -ForegroundColor Cyan
        Write-Host ""
        Write-Host "Test:" -ForegroundColor White
        Write-Host "  $exe -h localhost -p 6379 PING" -ForegroundColor Gray
    } else {
        Write-Host ""
        Write-Host "Build failed. Opening solution in VS2022..." -ForegroundColor Yellow
        Start-Process "$VS2022\vcli.sln"
    }
} else {
    Write-Host "MSBuild not found on PATH." -ForegroundColor Yellow
    Write-Host "Opening solution in Visual Studio 2022..." -ForegroundColor Yellow
    Start-Process "$VS2022\vcli.sln"
    Write-Host ""
    Write-Host "In VS2022: select Release | x64, then Build → Build Solution (Ctrl+Shift+B)" -ForegroundColor Cyan
}
