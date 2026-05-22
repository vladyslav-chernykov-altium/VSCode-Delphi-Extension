#Requires -Version 5.1
<#
.SYNOPSIS
    Installs build dependencies for rsm2pdb under c:/Dev/Tools and c:/Dev/Lib.

.DESCRIPTION
    Idempotent installer for:
      - MSYS2     -> c:/Dev/Tools/msys64/   (then: pacman -S mingw-w64-x86_64-gdb)
      - LLVM 18.1.8 (built from source with MSVC) -> c:/Dev/Lib/LLVM-18/

    CMake and Ninja are NOT installed by this script - they ship inside
    Visual Studio (under Common7\IDE\CommonExtensions\Microsoft\CMake).
    This script discovers them and adds them to the user PATH so they are
    available in any shell, not just the VS Developer Command Prompt.

    Visual Studio (2022 or 2026) with the 'Desktop development with C++'
    workload must be installed separately via the GUI installer. Both
    legacy (VS17 under Program Files (x86)) and modern (VS18 under
    Program Files) layouts are auto-detected.

    Run from a regular PowerShell prompt (not the VS Native Tools prompt -
    we invoke vcvars64.bat ourselves when needed).

.PARAMETER SkipLLVM
    Skip the LLVM source build (longest step, ~30-60min). Useful if you
    only want to install the small tools first.

.PARAMETER LLVMVersion
    LLVM git tag to build. Defaults to llvmorg-18.1.8.
#>

[CmdletBinding()]
param(
    [switch]$SkipLLVM,
    [string]$LLVMVersion = 'llvmorg-18.1.8'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# GitHub and msys2.org require TLS 1.2+; .NET's default may still negotiate
# the older protocols and get refused by the server.
[Net.ServicePointManager]::SecurityProtocol = `
    [Net.SecurityProtocolType]::Tls12 -bor `
    [Net.SecurityProtocolType]::Tls13

# ---------------------------------------------------------------------------
# Paths and versions (single source of truth)
# ---------------------------------------------------------------------------
$ToolsRoot   = 'C:\Dev\Tools'
$LibRoot     = 'C:\Dev\Lib'
$SrcRoot     = 'C:\Dev\Src'

# MSYS2 installer release date is looked up dynamically from GitHub
# (the asset is named msys2-x86_64-YYYYMMDD.exe and the date moves).

$MSYS2Dir   = Join-Path $ToolsRoot 'msys64'
$LLVMSrc    = Join-Path $SrcRoot   'llvm-project'
$LLVMBuild  = Join-Path $LLVMSrc   'build'
$LLVMInstall= Join-Path $LibRoot   'LLVM-18'

$DownloadCache = Join-Path $env:TEMP 'rsm2pdb-installs'

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
function Write-Step($msg)  { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)    { Write-Host "    $msg" -ForegroundColor Green }
function Write-Skip($msg)  { Write-Host "    $msg (already present, skipping)" -ForegroundColor DarkGray }

function Ensure-Dir($path) {
    if (-not (Test-Path $path)) {
        New-Item -ItemType Directory -Path $path -Force | Out-Null
    }
}

function Download-File($url, $dest) {
    if (Test-Path $dest) { Write-Skip "download $dest"; return }
    Write-Ok "downloading $url"
    # Try Invoke-WebRequest first (honors $ProgressPreference, follows
    # redirects, uses the TLS protocol we set above). Fall back to BITS,
    # then to WebClient as last resort.
    $tmp = "$dest.partial"
    if (Test-Path $tmp) { Remove-Item $tmp -Force }
    $prevProgress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'  # avoid IWR progress bar overhead
    try {
        try {
            Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -ErrorAction Stop
        } catch {
            Write-Ok "  IWR failed ($($_.Exception.Message)); trying BITS"
            try {
                Start-BitsTransfer -Source $url -Destination $tmp -ErrorAction Stop
            } catch {
                Write-Ok "  BITS failed ($($_.Exception.Message)); trying WebClient"
                $wc = New-Object System.Net.WebClient
                $wc.DownloadFile($url, $tmp)
            }
        }
        Move-Item -Path $tmp -Destination $dest -Force
    } finally {
        $ProgressPreference = $prevProgress
        if (Test-Path $tmp) { Remove-Item $tmp -Force -ErrorAction SilentlyContinue }
    }
}

function Add-UserPath($entry) {
    $current = [Environment]::GetEnvironmentVariable('Path', 'User')
    if ($null -eq $current) { $current = '' }
    $parts = $current.Split(';') | Where-Object { $_ -ne '' }
    if ($parts -notcontains $entry) {
        $new = ($parts + $entry) -join ';'
        [Environment]::SetEnvironmentVariable('Path', $new, 'User')
        Write-Ok "added to user PATH: $entry"
    } else {
        Write-Skip "PATH entry $entry"
    }
    # Also update current session so subsequent steps see it.
    if (-not ($env:Path -split ';' -contains $entry)) {
        $env:Path = "$env:Path;$entry"
    }
}

function Find-VSInstall {
    # Try legacy vswhere first (works for VS2017/2019/2022 - VS15/16/17).
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -prerelease -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($vsRoot -and (Test-Path (Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'))) {
            return $vsRoot
        }
    }

    # Fallback for VS2026 (VS18) and any future version under 64-bit
    # Program Files. Legacy x86 vswhere does not see these.
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio'
    )
    foreach ($base in $candidates) {
        if (-not (Test-Path $base)) { continue }
        # Newest version directory first (numeric sort).
        $verDirs = Get-ChildItem $base -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+$' } |
            Sort-Object { [int]$_.Name } -Descending
        foreach ($verDir in $verDirs) {
            # Editions: Insiders, Community, Professional, Enterprise, BuildTools, Preview, ...
            $editions = Get-ChildItem $verDir.FullName -Directory -ErrorAction SilentlyContinue
            foreach ($ed in $editions) {
                $vcvars = Join-Path $ed.FullName 'VC\Auxiliary\Build\vcvars64.bat'
                if (Test-Path $vcvars) { return $ed.FullName }
            }
        }
    }

    throw "No Visual Studio install with x64 C++ tools found. Install VS 2022 or 2026 with the 'Desktop development with C++' workload. Download: https://visualstudio.microsoft.com/downloads/"
}

function Find-VCVarsAll {
    $vsRoot = Find-VSInstall
    $vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found under $vsRoot"
    }
    return $vcvars
}

function Find-VSCMakeAndNinja {
    # CMake and Ninja that ship with VS, so we can put them on PATH for
    # use outside the VS Developer Command Prompt.
    $vsRoot = Find-VSInstall
    $base = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $cmakeBin = Join-Path $base 'CMake\bin'
    $ninjaBin = Join-Path $base 'Ninja'
    $result = @{}
    if (Test-Path (Join-Path $cmakeBin 'cmake.exe')) { $result.CMake = $cmakeBin }
    if (Test-Path (Join-Path $ninjaBin 'ninja.exe')) { $result.Ninja = $ninjaBin }
    return $result
}

function Invoke-InVCEnv([string]$workDir, [string[]]$commands) {
    $vcvars = Find-VCVarsAll
    Push-Location $workDir
    try {
        $joined = $commands -join ' && '
        & cmd /c "`"$vcvars`" >NUL && $joined"
        if ($LASTEXITCODE -ne 0) {
            throw "Command failed under vcvars64 (exit $LASTEXITCODE): $joined"
        }
    } finally {
        Pop-Location
    }
}

# ---------------------------------------------------------------------------
# 0. Prep
# ---------------------------------------------------------------------------
Write-Step "Preparing directories"
Ensure-Dir $ToolsRoot
Ensure-Dir $LibRoot
Ensure-Dir $SrcRoot
Ensure-Dir $DownloadCache

Write-Step "Verifying Visual Studio C++ toolchain"
$vcvars = Find-VCVarsAll
Write-Ok "found vcvars64.bat at $vcvars"

# ---------------------------------------------------------------------------
# 1. CMake + Ninja (use the copies bundled with Visual Studio)
# ---------------------------------------------------------------------------
Write-Step "Publishing VS-bundled CMake and Ninja to user PATH"
$vsTools = Find-VSCMakeAndNinja
if ($vsTools.ContainsKey('CMake')) {
    Add-UserPath $vsTools.CMake
    Write-Ok "CMake found at $($vsTools.CMake)"
} else {
    throw "VS-bundled CMake not found. Re-run the VS Installer and add the 'C++ CMake tools for Windows' individual component."
}
if ($vsTools.ContainsKey('Ninja')) {
    Add-UserPath $vsTools.Ninja
    Write-Ok "Ninja found at $($vsTools.Ninja)"
} else {
    throw "VS-bundled Ninja not found. Same fix: add 'C++ CMake tools for Windows'."
}

# ---------------------------------------------------------------------------
# 3. Git sanity check
# ---------------------------------------------------------------------------
Write-Step "Checking for Git"
$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) {
    throw "Git not found in PATH. Install Git for Windows from https://git-scm.com/download/win and re-run."
}
Write-Ok "git: $($git.Source)"

# ---------------------------------------------------------------------------
# 4. MSYS2 (for gdb)
# ---------------------------------------------------------------------------
Write-Step "Installing MSYS2 + mingw-w64 gdb"
$msys2Bash = Join-Path $MSYS2Dir 'usr\bin\bash.exe'
if (Test-Path $msys2Bash) {
    Write-Skip "MSYS2 at $MSYS2Dir"
} else {
    Write-Ok "querying GitHub for latest MSYS2 installer release"
    $apiUrl = 'https://api.github.com/repos/msys2/msys2-installer/releases/latest'
    $headers = @{ 'User-Agent' = 'rsm2pdb-installer' }
    $release = Invoke-RestMethod -Uri $apiUrl -Headers $headers -UseBasicParsing
    # MSYS2 currently ships its installer under a rolling 'nightly-x86_64'
    # tag, with the asset named msys2-x86_64-latest.exe. Older dated
    # releases used msys2-x86_64-YYYYMMDD.exe. Accept either.
    $asset = $release.assets |
        Where-Object { $_.name -match '^msys2-x86_64-(latest|\d{8})\.exe$' } |
        Select-Object -First 1
    if (-not $asset) {
        $names = ($release.assets | ForEach-Object { $_.name }) -join ', '
        throw "No msys2-x86_64 installer asset found. Release: $($release.tag_name). Assets: $names"
    }
    Write-Ok "release tag: $($release.tag_name)  asset: $($asset.name)"

    $installerPath = Join-Path $DownloadCache $asset.name
    Download-File $asset.browser_download_url $installerPath

    Write-Ok "running MSYS2 unattended installer (this opens a brief progress window)"
    & $installerPath install --confirm-command --accept-messages --root $MSYS2Dir | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "MSYS2 installer exited with code $LASTEXITCODE" }
    if (-not (Test-Path $msys2Bash)) {
        throw "MSYS2 installer reported success but $msys2Bash is missing."
    }
}

# Initial system update + install gdb. MSYS2's first -Syuu may close the shell;
# we run it twice to converge.
$gdbExe = Join-Path $MSYS2Dir 'mingw64\bin\gdb.exe'
if (Test-Path $gdbExe) {
    Write-Skip "gdb at $gdbExe"
} else {
    Write-Ok "updating MSYS2 packages (first pass)"
    & $msys2Bash -lc "pacman -Syuu --noconfirm" | Out-Host
    Write-Ok "updating MSYS2 packages (second pass)"
    & $msys2Bash -lc "pacman -Syuu --noconfirm" | Out-Host
    Write-Ok "installing mingw-w64-x86_64-gdb"
    & $msys2Bash -lc "pacman -S --noconfirm mingw-w64-x86_64-gdb" | Out-Host
    if (-not (Test-Path $gdbExe)) {
        throw "gdb still missing at $gdbExe after pacman install"
    }
    Write-Ok "gdb installed at $gdbExe"
}
Add-UserPath (Join-Path $MSYS2Dir 'mingw64\bin')

# ---------------------------------------------------------------------------
# 5. LLVM 18.1.8 from source
# ---------------------------------------------------------------------------
if ($SkipLLVM) {
    Write-Step "Skipping LLVM build (-SkipLLVM)"
} else {
    Write-Step "Building LLVM $LLVMVersion from source (this takes 30-60 minutes)"
    $llvmConfigCmake = Join-Path $LLVMInstall 'lib\cmake\llvm\LLVMConfig.cmake'
    if (Test-Path $llvmConfigCmake) {
        Write-Skip "LLVM at $LLVMInstall"
    } else {
        Ensure-Dir $SrcRoot
        if (-not (Test-Path $LLVMSrc)) {
            Write-Ok "cloning llvm-project @ $LLVMVersion"
            & git clone --depth 1 --branch $LLVMVersion `
                https://github.com/llvm/llvm-project.git $LLVMSrc | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
        } else {
            Write-Skip "llvm-project source tree at $LLVMSrc"
        }

        Ensure-Dir $LLVMBuild
        # "Lean" profile: drop tools, utils, optional deps, dylibs.
        # We only consume Support / Object / BinaryFormat / MC /
        # DebugInfoDWARF as static libs; everything else is dead weight.
        $configureCmd = @(
            'cmake',
            '-S', "`"$LLVMSrc\llvm`"",
            '-B', "`"$LLVMBuild`"",
            '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release',
            "-DCMAKE_INSTALL_PREFIX=`"$LLVMInstall`"",
            '-DLLVM_TARGETS_TO_BUILD=X86',
            '-DLLVM_ENABLE_RTTI=ON',
            '-DLLVM_ENABLE_EH=ON',
            '-DLLVM_BUILD_TOOLS=OFF',
            '-DLLVM_BUILD_UTILS=OFF',
            '-DLLVM_BUILD_RUNTIME=OFF',
            '-DLLVM_BUILD_LLVM_DYLIB=OFF',
            '-DLLVM_BUILD_LLVM_C_DYLIB=OFF',
            '-DLLVM_INCLUDE_TOOLS=OFF',
            '-DLLVM_INCLUDE_UTILS=OFF',
            '-DLLVM_INCLUDE_TESTS=OFF',
            '-DLLVM_INCLUDE_EXAMPLES=OFF',
            '-DLLVM_INCLUDE_BENCHMARKS=OFF',
            '-DLLVM_ENABLE_BACKTRACES=OFF',
            '-DLLVM_ENABLE_TERMINFO=OFF',
            '-DLLVM_ENABLE_LIBXML2=OFF',
            '-DLLVM_ENABLE_ZSTD=OFF',
            '-DLLVM_ENABLE_ZLIB=OFF',
            '-DLLVM_ENABLE_LIBEDIT=OFF',
            '-DLLVM_ENABLE_LIBPFM=OFF',
            '-DLLVM_ENABLE_BINDINGS=OFF',
            '-DLLVM_ENABLE_OCAMLDOC=OFF'
        ) -join ' '

        $buildCmd   = "cmake --build `"$LLVMBuild`" --target install"

        Invoke-InVCEnv -workDir $SrcRoot -commands @($configureCmd, $buildCmd)
        Write-Ok "LLVM installed to $LLVMInstall"
    }
}

# ---------------------------------------------------------------------------
# 6. Summary
# ---------------------------------------------------------------------------
Write-Step "Done"
Write-Host ""
Write-Host "Installed / discovered paths:" -ForegroundColor White
Write-Host "  CMake : $($vsTools.CMake)   (VS-bundled)"
Write-Host "  Ninja : $($vsTools.Ninja)   (VS-bundled)"
Write-Host "  MSYS2 : $MSYS2Dir"
Write-Host "  gdb   : $(Join-Path $MSYS2Dir 'mingw64\bin\gdb.exe')"
Write-Host "  LLVM  : $LLVMInstall"
Write-Host ""
Write-Host "Open a NEW PowerShell window so the updated user PATH takes effect," -ForegroundColor Yellow
Write-Host "then from the rsm2pdb workspace run:"                                  -ForegroundColor Yellow
Write-Host "  cmake -S . -B build -G Ninja -DLLVM_DIR=`"$LLVMInstall/lib/cmake/llvm`""
Write-Host "  cmake --build build"
