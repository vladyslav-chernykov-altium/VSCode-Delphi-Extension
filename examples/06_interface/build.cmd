@echo off
REM Build iface.dproj and inject PDB. Companion fixture for the
REM interface-dispatch debugger UX work -- shows direct calls,
REM IMyInterface(Pointer) raw casts, class-as-iface, and chained
REM casts so we can verify rsm2pdb's step-into / auto-skip on each.

setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set REPO_ROOT=%~dp0..\..
set OUT=Win64\%CONFIG%
set RSM2PDB=%REPO_ROOT%\build\src\rsm2pdb.exe

call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat"
if errorlevel 1 ( echo rsvars.bat failed & exit /b 1 )

msbuild iface.dproj /t:Rebuild /p:Config=%CONFIG% /p:Platform=Win64 /nologo /v:minimal
if errorlevel 1 ( echo MSBuild failed & exit /b 1 )

if not exist "%OUT%\iface.exe" ( echo no exe produced & exit /b 1 )
if not exist "%OUT%\iface.map" ( echo no map produced & exit /b 1 )

copy /Y "%OUT%\iface.exe" "%OUT%\iface_orig.exe" >nul
"%RSM2PDB%" pdb "%OUT%\iface.map" "%OUT%\iface.exe" "%OUT%\iface.pdb"
if errorlevel 1 ( echo rsm2pdb failed & exit /b 1 )

echo Built %OUT%\iface.exe with PDB.
exit /b 0
