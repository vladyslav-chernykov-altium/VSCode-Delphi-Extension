# rsm2pdb

Debug **Delphi** Win64 binaries in **VSCode** (or any gdb front-end) by
converting Delphi's debug-info output into DWARF and injecting it back
into the executable.

## What works today (Milestone M2 phase B-lite)

| Capability | Status |
|---|---|
| Breakpoint by clicking the gutter in a `.pas` / `.dpr` file | ✅ |
| Step over / step into / step out, line by line | ✅ |
| Call Stack shows source file, line number, and Delphi function name | ✅ |
| `break Geometry.Add` works at the gdb prompt | ✅ |
| Multi-unit projects (`uses` across multiple `.pas` files) | ✅ |
| Dotted unit names (`App.Colors`) | ✅ |
| Global variables shown as their Pascal primitive type (`S: Integer`) | ✅ |
| Records / enums / classes shown as `byte[N]` arrays (fallback) | ✅ |
| **Locals / parameters / record fields** | ⏳ M2 phase A |
| Precise typing for same-size primitives (Cardinal vs Integer, Double vs Int64) | ⏳ see below |

### Known display quirks

Because the per-unit type-binding table inside the modern `.rsm` is not
yet fully decoded (work in progress — see `rsm-format.txt`), some
primitive globals display under their best-fit same-size sibling:

| You declared | Debugger shows | Why |
|---|---|---|
| `Cardinal`        | `Integer` | both 4 bytes, sign info merged |
| `Single`          | `Int64`   | 4-byte float padded to 8, lands on the 8-byte slot |
| `Double` / `UInt64` | `Int64` | both 8 bytes, no sign / float-vs-int signal |
| `Byte` / `ShortInt` / `Boolean` | `Word` | 1-byte types padded to 2 by alignment |

**The bytes are always correct;** only the type *name* is best-effort.
Two practical workarounds:

**A. One-off check in VSCode Debug Console** (no setup needed):

```
-exec print/f gD          ⇒  2.5         (Double — 8-byte float view)
-exec print/u gC          ⇒  2           (Cardinal — unsigned view)
-exec print/x gD          ⇒  0x4004000000000000   (raw bytes)
```

Available gdb format specifiers: `/f` float, `/d` signed dec, `/u`
unsigned dec, `/x` hex, `/o` octal, `/t` binary.

**B. Persistent Watch entries** (one-time setup per session). gdb's
default Pascal mode rejects C pointer-cast syntax, so first switch
the expression language in Debug Console:

```
-exec set language c
```

Then in the VSCode Watch panel, add:

```
*(double*)&gD        ⇒  2.5
*(float*)&gF         ⇒  1.5
(unsigned)gC         ⇒  2
```

These update live as you step. gdb prints one "language does not
match frame" warning on language switch — cosmetic, ignore.

*Why the comma-format syntax (`gD,f`) doesn't work:* that's a
Visual Studio cppvsdbg feature; the gdb-backed cppdbg adapter
doesn't translate it.

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
