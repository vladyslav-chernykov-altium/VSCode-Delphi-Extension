#!/bin/bash
# Build rsm2pdb + rsm2pdb_tests in Debug via MSVC + Ninja.
# Replaces the 200-char vcvars-wrap incantation. Pipe to tail/grep
# as needed (the script itself prints raw cmake --build output).
exec cmd.exe /C 'call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && cd /d c:\Dev\Src\rsm2pdb\build && cmake --build . --config Debug --target rsm2pdb rsm2pdb_tests'
