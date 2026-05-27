@echo off
REM Build locals.dproj and inject PDB debug info into the EXE.
REM Companion fixture for RSM proc-record reverse engineering --
REM exercises every common Delphi function shape (global proc/func,
REM class method, virtual, override, overload, 0..10 params, locals).

setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set REPO_ROOT=%~dp0..\..
set OUT=Win64\%CONFIG%
set RSM2PDB=%REPO_ROOT%\build\src\rsm2pdb.exe

call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat"
if errorlevel 1 ( echo rsvars.bat failed & exit /b 1 )

msbuild locals.dproj /t:Rebuild /p:Config=%CONFIG% /p:Platform=Win64 /nologo /v:minimal
if errorlevel 1 ( echo MSBuild failed & exit /b 1 )

if not exist "%OUT%\locals.exe" ( echo no exe produced & exit /b 1 )
if not exist "%OUT%\locals.map" ( echo no map produced & exit /b 1 )

copy /Y "%OUT%\locals.exe" "%OUT%\locals_orig.exe" >nul

"%RSM2PDB%" pdb "%OUT%\locals.map" "%OUT%\locals.exe" "%OUT%\locals.pdb"
if errorlevel 1 ( echo rsm2pdb failed & exit /b 1 )

echo Built %OUT%\locals.exe with PDB.
exit /b 0
