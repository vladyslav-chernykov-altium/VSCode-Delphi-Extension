@echo off
REM Build primitives.dproj and inject DWARF debug info into the EXE.
REM
REM Usage: build.cmd [Debug|Release]   (default Debug)
REM
REM Pipeline:
REM   1. MSBuild (Delphi)  -> Win64\<Config>\primitives.exe (+ .rsm + .map)
REM   2. rsm2pdb dwarf     -> replaces primitives.exe with a DWARF-
REM                           enriched copy. The original is kept as
REM                           primitives_orig.exe.

setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set REPO_ROOT=%~dp0..\..
set OUT=Win64\%CONFIG%
set RSM2PDB=%REPO_ROOT%\build\src\rsm2pdb.exe

REM -------- 1. Delphi MSBuild ----------
call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat"
if errorlevel 1 (
    echo rsvars.bat failed
    exit /b 1
)

msbuild primitives.dproj /t:Rebuild /p:Config=%CONFIG% /p:Platform=Win64 /nologo /v:minimal
if errorlevel 1 (
    echo MSBuild failed
    exit /b 1
)

REM -------- 2. DWARF injection ----------
if not exist "%RSM2PDB%" (
    echo error: rsm2pdb.exe not built. Run cmake --build build first.
    exit /b 1
)

move /Y "%OUT%\primitives.exe" "%OUT%\primitives_orig.exe" 1>nul
"%RSM2PDB%" dwarf "%OUT%\primitives.map" ^
                  "%OUT%\primitives_orig.exe" ^
                  "%OUT%\primitives.exe"
if errorlevel 1 (
    echo DWARF injection failed - restoring original.
    move /Y "%OUT%\primitives_orig.exe" "%OUT%\primitives.exe" 1>nul
    exit /b 1
)

echo.
echo === Build output ===
dir /b %OUT%\primitives.exe %OUT%\primitives.rsm %OUT%\primitives.map %OUT%\primitives_orig.exe 2>nul

endlocal
