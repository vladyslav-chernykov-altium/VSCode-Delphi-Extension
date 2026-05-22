#Requires -Version 5.1
<#
.SYNOPSIS
    Build a Delphi project with DWARF debug info injected by rsm2pdb.

.DESCRIPTION
    1. Build the .dproj via Delphi's MSBuild.
    2. Inject DWARF (from the produced .map) into the .exe via rsm2pdb.
    3. Print the path of the debuggable exe (caller / VSCode launch
       config consumes this).

    Skeleton helper - not heavily tested. Intended as the seam VSCode
    tasks call into, and as the basis for the planned vscode-ext.

.PARAMETER Dproj
    Path to a .dproj file. Defaults to the first .dproj found in the
    current directory.

.PARAMETER Config
    "Debug" or "Release". Default Debug.

.PARAMETER Platform
    "Win64" or "Win32". Default Win64 (rsm2pdb only supports Win64 today).

.PARAMETER Bds
    Embarcadero Studio install path. Default
    C:\Dev\Tools\Embarcadero\Studio\19.0 (Delphi 10.2 Tokyo).

.PARAMETER Rsm2pdb
    Path to rsm2pdb.exe. Default resolved relative to this script.

.EXAMPLE
    .\scripts\delphi-debug.ps1 -Dproj examples\02_two_units\two_units.dproj
#>

[CmdletBinding()]
param(
    [string]$Dproj    = "",
    [ValidateSet("Debug","Release")][string]$Config   = "Debug",
    [ValidateSet("Win64","Win32")] [string]$Platform = "Win64",
    [string]$Bds      = "C:\Dev\Tools\Embarcadero\Studio\19.0",
    [string]$Rsm2pdb  = ""
)

$ErrorActionPreference = 'Stop'

# Resolve .dproj if not given.
if (-not $Dproj) {
    $found = Get-ChildItem -Path . -Filter *.dproj -File -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if (-not $found) {
        Write-Host "no .dproj found in $(Get-Location); pass -Dproj <path>" -ForegroundColor Red
        exit 2
    }
    $Dproj = $found.FullName
}
if (-not (Test-Path $Dproj)) {
    Write-Host "dproj not found: $Dproj" -ForegroundColor Red
    exit 2
}

# Resolve rsm2pdb if not given.
if (-not $Rsm2pdb) {
    $repoRoot = (Get-Item $PSScriptRoot).Parent.FullName
    $Rsm2pdb  = Join-Path $repoRoot 'build\src\rsm2pdb.exe'
}
if (-not (Test-Path $Rsm2pdb)) {
    Write-Host "rsm2pdb.exe not built at $Rsm2pdb" -ForegroundColor Red
    Write-Host "  run: cmake --build build" -ForegroundColor Yellow
    exit 2
}

$dprojDir  = Split-Path -Parent $Dproj
$projName  = [IO.Path]::GetFileNameWithoutExtension($Dproj)
$outDir    = Join-Path $dprojDir "$Platform\$Config"

Write-Host "==> Delphi MSBuild: $projName $Platform $Config" -ForegroundColor Cyan

$rsvars = Join-Path $Bds 'bin\rsvars.bat'
if (-not (Test-Path $rsvars)) {
    Write-Host "rsvars.bat not found at $rsvars" -ForegroundColor Red
    exit 2
}

# Build via cmd so rsvars.bat takes effect.
$build = "call `"$rsvars`" && msbuild `"$Dproj`" /t:Rebuild /p:Config=$Config /p:Platform=$Platform /nologo /v:minimal"
& cmd /c $build
if ($LASTEXITCODE -ne 0) {
    Write-Host "MSBuild failed (exit $LASTEXITCODE)" -ForegroundColor Red
    exit $LASTEXITCODE
}

$exePath  = Join-Path $outDir "$projName.exe"
$mapPath  = Join-Path $outDir "$projName.map"
$origPath = Join-Path $outDir "${projName}_orig.exe"

if (-not (Test-Path $exePath)) {
    Write-Host "expected exe not produced: $exePath" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $mapPath)) {
    Write-Host "no .map produced. Check DCC_MapFile=3 in dproj." -ForegroundColor Red
    exit 1
}

Write-Host "==> Injecting DWARF" -ForegroundColor Cyan
Move-Item -Force $exePath $origPath
& $Rsm2pdb dwarf $mapPath $origPath $exePath
if ($LASTEXITCODE -ne 0) {
    Write-Host "rsm2pdb dwarf failed - restoring original" -ForegroundColor Red
    Move-Item -Force $origPath $exePath
    exit $LASTEXITCODE
}

Write-Host "==> Done" -ForegroundColor Green
Write-Host "  debuggable: $exePath"
