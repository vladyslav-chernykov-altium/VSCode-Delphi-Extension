@echo off
REM Build inherit_props.dproj and inject PDB.  Probes 3-level
REM class inheritance + Pascal property handling.

setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set REPO_ROOT=%~dp0..\..
set OUT=Win64\%CONFIG%
set RSM2PDB=%REPO_ROOT%\build\src\rsm2pdb.exe

call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat"
if errorlevel 1 ( echo rsvars.bat failed & exit /b 1 )

msbuild inherit_props.dproj /t:Rebuild /p:Config=%CONFIG% /p:Platform=Win64 /nologo /v:minimal
if errorlevel 1 ( echo MSBuild failed & exit /b 1 )

if not exist "%OUT%\inherit_props.exe" ( echo no exe produced & exit /b 1 )
if not exist "%OUT%\inherit_props.map" ( echo no map produced & exit /b 1 )

copy /Y "%OUT%\inherit_props.exe" "%OUT%\inherit_props_orig.exe" >nul
"%RSM2PDB%" pdb "%OUT%\inherit_props.map" "%OUT%\inherit_props.exe" "%OUT%\inherit_props.pdb"
if errorlevel 1 ( echo rsm2pdb failed & exit /b 1 )

echo Built %OUT%\inherit_props.exe with PDB.
exit /b 0
