# rsm2pdb

Debug **Delphi** Win64 binaries in **VSCode** (or any gdb front-end) by
converting Delphi's debug-info output into DWARF and injecting it back
into the executable.

## What works today (Milestone M1)

| Capability | Status |
|---|---|
| Breakpoint by clicking the gutter in a `.pas` / `.dpr` file | ✅ |
| Step over / step into / step out, line by line | ✅ |
| Call Stack shows source file, line number, and Delphi function name | ✅ |
| `break Geometry.Add` works at the gdb prompt | ✅ |
| Multi-unit projects (`uses` across multiple `.pas` files) | ✅ |
| Dotted unit names (`App.Colors`) | ✅ |
| **Variable inspection (locals / params / record fields)** | ⏳ Not yet (needs M2) |

The remaining gap — variable inspection — needs the `.rsm` parser
(reverse engineering, planned). Lines + symbols already give a usable
debug experience.

## How it works

```
Delphi source (*.pas, *.dpr)
        │
        │  Delphi MSBuild
        ▼
hello.exe + hello.rsm + hello.map
        │
        │  rsm2pdb dwarf hello.map hello.exe hello_dbg.exe
        ▼
hello_dbg.exe                                  <- has DWARF v5 sections injected
        │
        ▼  gdb (mingw-w64) ----------------- VSCode cppdbg adapter
```

Internally:

- The **.map** file is parsed for line tables, public symbols, and
  segment layout (`src/map/`).
- A **`model::Module`** intermediate representation holds source files,
  CompileUnits, Symbols, LineEntry tuples (`src/model/`).
- A DWARF v5 emitter writes `.debug_info`, `.debug_abbrev`,
  `.debug_line` byte streams (`src/dwarf/`).
- A PE injector appends those bytes as new sections to a copy of the
  EXE, patching headers as needed (`src/pe/`).
- The `.rsm` parser (`src/rsm/`) is a stub today; it's what M2 needs
  for full variable / type inspection.

## Prerequisites

- **Visual Studio 2022 or 2026** with the *Desktop development with C++*
  workload (provides `cl.exe`, CMake, Ninja).
- **Delphi** with the dcc64 / MSBuild toolchain on PATH (we tested with
  Delphi 10.2 Tokyo at `C:\Dev\Tools\Embarcadero\Studio\19.0\`).
- **MSYS2** with `mingw-w64-x86_64-gdb` installed (`C:\Dev\Tools\msys64\`
  by convention).
- **LLVM 18.1.x** built from source against MSVC (`C:\Dev\Lib\LLVM-18\`).
- The one-time installer script `scripts/install-deps.ps1` handles
  MSYS2 + LLVM if you want to follow the convention.

## Build

From a regular PowerShell (not the VS Dev Command Prompt — the build
sources `vcvars64.bat` itself when needed):

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DLLVM_DIR=C:/Dev/Lib/LLVM-18/lib/cmake/llvm
cmake --build build
ctest --test-dir build --output-on-failure
```

Resulting binary: `build\src\rsm2pdb.exe`.

## Use on a Delphi project

The simplest path (mirrors `examples/02_two_units/build.cmd`):

1. Ensure your `.dproj` has these compiler options:
   ```xml
   <DCC_DebugInformation>1</DCC_DebugInformation>
   <DCC_LocalDebugSymbols>true</DCC_LocalDebugSymbols>
   <DCC_MapFile>3</DCC_MapFile>            <!-- detailed map -->
   <DCC_DebugInfoInExe>false</DCC_DebugInfoInExe>  <!-- frees PE header room -->
   ```
2. Build via Delphi MSBuild:
   ```cmd
   call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat"
   msbuild MyProject.dproj /p:Config=Debug /p:Platform=Win64
   ```
3. Inject DWARF:
   ```cmd
   move /Y Win64\Debug\MyProject.exe Win64\Debug\MyProject_orig.exe
   rsm2pdb dwarf Win64\Debug\MyProject.map ^
                 Win64\Debug\MyProject_orig.exe ^
                 Win64\Debug\MyProject.exe
   ```
4. Add a VSCode launch config (see `.vscode/launch.json` for a working
   template). The key bits:
   ```json
   {
     "type": "cppdbg",
     "MIMode": "gdb",
     "miDebuggerPath": "C:/Dev/Tools/msys64/mingw64/bin/gdb.exe",
     "program": "...path to your DWARF-enriched .exe...",
     "sourceFileMap": {
       "MyUnit.pas": "${workspaceFolder}/src/MyUnit.pas"
     }
   }
   ```
5. In `.vscode/settings.json`:
   ```json
   { "debug.allowBreakpointsEverywhere": true }
   ```
   (cppdbg's default whitelist doesn't include Pascal.)

## Examples in this repo

| Path | What it shows |
|---|---|
| `examples/01_hello/` | Single-file Delphi program with a record + enum + function |
| `examples/02_two_units/` | Multi-unit project with a dotted unit name (`App.Colors`) |

Each has a `build.cmd` that runs the full Delphi-MSBuild → `rsm2pdb dwarf`
pipeline and produces a debuggable `.exe`.

## Repository layout

```
src/
  rsm/      - .rsm parser (stub today; M2 work)
  map/      - .map parser
  model/    - debugger-agnostic intermediate model
  dwarf/    - DWARF v5 emitter
  pe/       - PE section injector
  main.cpp  - CLI

test/
  unit/      - doctest unit + integration tests
  fixtures/  - committed .exe/.rsm/.map sample inputs

examples/   - Delphi sample projects
docs/       - architecture and design notes
scripts/    - dependency installer
spike/      - de-risking spikes (C reference); see spike/README.md
rsm-format.txt - reverse-engineering notebook for the .rsm format
todo.txt    - project status log and roadmap
```

## Roadmap

- **M2: Variable inspection.** Parse `.rsm` for types, locals, params.
  Emit `DW_TAG_structure_type`, `DW_TAG_variable`,
  `DW_TAG_formal_parameter`. ~weeks of reverse-engineering the modern
  Embarcadero "CSH7" RSM format.
- **M3: PDB backend.** Targets `cppvsdbg` / WinDbg / x64dbg / IDA Pro
  via Microsoft's PDB format. Reuses the same `model::Module`.

## License

TBD.
