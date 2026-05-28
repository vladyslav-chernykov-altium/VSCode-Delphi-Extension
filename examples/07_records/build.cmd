@echo off
REM Build records.dproj and inject PDB. Fixture for the
REM TPI-struct-synthesis work (Step 11b: records / classes / enums).

setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set REPO_ROOT=%~dp0..\..
set OUT=Win64\%CONFIG%
set RSM2PDB=%REPO_ROOT%\build\src\rsm2pdb.exe

call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat"
if errorlevel 1 ( echo rsvars.bat failed & exit /b 1 )

msbuild records.dproj /t:Rebuild /p:Config=%CONFIG% /p:Platform=Win64 /nologo /v:minimal
if errorlevel 1 ( echo MSBuild failed & exit /b 1 )

if not exist "%OUT%\records.exe" ( echo no exe produced & exit /b 1 )
if not exist "%OUT%\records.map" ( echo no map produced & exit /b 1 )

copy /Y "%OUT%\records.exe" "%OUT%\records_orig.exe" >nul
"%RSM2PDB%" pdb "%OUT%\records.map" "%OUT%\records.exe" "%OUT%\records.pdb"
if errorlevel 1 ( echo rsm2pdb failed & exit /b 1 )

echo Built %OUT%\records.exe with PDB.
exit /b 0
