@echo off
REM Build cross_unit.dproj (multi-unit) and inject PDB.  Fixture
REM for the cross-unit type-reference resolution work -- the
REM consuming unit's RSM only carries 0x66 NAME entries for the
REM foreign types so field rendering needs to look globally.

setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set REPO_ROOT=%~dp0..\..
set OUT=Win64\%CONFIG%
set RSM2PDB=%REPO_ROOT%\build\src\rsm2pdb.exe

call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat"
if errorlevel 1 ( echo rsvars.bat failed & exit /b 1 )

msbuild cross_unit.dproj /t:Rebuild /p:Config=%CONFIG% /p:Platform=Win64 /nologo /v:minimal
if errorlevel 1 ( echo MSBuild failed & exit /b 1 )

if not exist "%OUT%\cross_unit.exe" ( echo no exe produced & exit /b 1 )
if not exist "%OUT%\cross_unit.map" ( echo no map produced & exit /b 1 )

copy /Y "%OUT%\cross_unit.exe" "%OUT%\cross_unit_orig.exe" >nul
"%RSM2PDB%" pdb "%OUT%\cross_unit.map" "%OUT%\cross_unit.exe" "%OUT%\cross_unit.pdb"
if errorlevel 1 ( echo rsm2pdb failed & exit /b 1 )

echo Built %OUT%\cross_unit.exe with PDB.
exit /b 0
