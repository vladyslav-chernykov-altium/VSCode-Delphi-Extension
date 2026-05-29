# Architecture

```
.pas / .dpr ─┐
              │  Delphi MSBuild  (orchestrated by vscode-ext/ or
              │                  examples/*/build.cmd or manually)
              ▼
       <project>.exe/.dll + <project>.rsm + <project>.map
              │
              ▼
   ┌─────────────────────┐    ┌─────────────────────┐
   │  src/map/           │    │  src/rsm/           │
   │  text parser        │    │  binary parser      │
   │  + populate adapter │    │  + decorateTypes    │
   └──────────┬──────────┘    └──────────┬──────────┘
              │                          │
              │     populate()           │  decorateTypes()
              ▼                          ▼
                  ┌─────────────────────┐
                  │   model::Module     │
                  │ (debugger-agnostic) │
                  │ source_files,       │
                  │ CompileUnits,       │
                  │ Symbols (with       │
                  │   params + locals), │
                  │ LineEntry,          │
                  │ Type (Primitive,    │
                  │   Array, ...)       │
                  └──────────┬──────────┘
                             │
                             ▼
                  ┌─────────────────────┐
                  │   src/dwarf/        │
                  │   DWARF v5 emitter  │
                  │ .debug_info,        │
                  │ .debug_abbrev,      │
                  │ .debug_line         │
                  └──────────┬──────────┘
                             │
                             ▼
                  ┌─────────────────────┐
                  │   src/pe/           │
                  │   PE section        │
                  │   injector          │
                  └──────────┬──────────┘
                             │
                             ▼
              <project>.exe/.dll  (DWARF-enriched PE/COFF)
                             │
                             ▼
                       gdb (mingw-w64)
                             │
                             ▼
                     VSCode cppdbg adapter
                             │
                             ▼
                       vscode-ext/   (the rsm2pdb extension)
```

## Layer responsibilities

### src/map/ — Delphi .map parser

Parses Delphi's detailed map file (DCC_MapFile=3). Section-aware
state machine recognising:

- Segments overview (`.text` at 0x401000, `.data`, `.bss`, ...)
- Detailed map of segments (per-module address ranges)
- Publics by Name / by Value
- Line numbers for `<unit>(<source>) segment <name>` (one block per
  user unit)
- Program entry point

Adapter `populate(MapFile, model::Module)` converts the raw map view
into the debugger-agnostic IR. Designed to scale to multi-million-
public real-world projects via two pre-bucketing passes:

1. Publics get bucketed by their LONGEST registered-unit-name prefix
   (avoids `App.Colors.X` getting added to both an `App` and an
   `App.Colors` CU).
2. Line tables get merged by module so each unique unit produces
   exactly ONE CompileUnit, with line entries from all its segments
   (.text / .data / .pdata).

Without these, Altium AdvPCB (946k publics × 38k line tables) produced
54 million duplicated symbols and took >80s in populate; after, it's
313k legitimate symbols in 1.2s.

Other behaviours:

- Computes absolute VAs from segment-relative offsets.
- Keeps Delphi-qualified names (`Geometry.Add`, not bare `Add`) so
  stack traces and `break X.Y` work as users expect.
- Strips the module prefix for variables (gdb's Pascal expression
  parser treats `.` as field access, so `Geometry.S` fails as a
  watch — we keep just `S`).
- Filters out linker-generated EH aux symbols (`$pdata$`, `$unwind$`)
  and anonymous compiler-internal symbols.

### src/rsm/ — Delphi .rsm parser

Modern Embarcadero "CSH7"-magic format, largely reverse-engineered
from scratch. Currently parses:

- **Header** (8 uint32 fields + EXE-path string).
- **Primitive type table** (28 built-in Delphi types, anchored on
  the `Boolean` record's unique byte signature, stable per-Delphi-
  install type-ids).
- **Variable records** (tag 0x20): three sub-forms (5-byte plain
  primitive, 12-byte extended primitive with source-position hash,
  6-byte non-primitive with inline type-id). VA encoded as
  `actual = stored >> 4`.
- **Procedure records** (tag 0x28): name + VA + return-type marker
  + parameter sub-records (tag 0x21) + local sub-records (tag 0x20)
  + end marker (0x63). Stack offsets carried in two forms (1-byte
  signed i8 LSB=0, 2-byte signed i16 LSB=1). The frame interpretation
  -- mapping the raw RSM offset onto a real rbp-relative address --
  lives entirely in `src/compose/` (see below).

VA-keyed lookup indexes (`unordered_map<VA, idx>`) for variables and
procedures keep cross-referencing O(1) at million-symbol scale.

Adapter `decorateTypes(reader, mod)` populates Symbol.params /
Symbol.locals from RSM procedure records, applying the M2-phase-B-lite
size-heuristic for primitive type resolution.

See `docs/02-rsm-format-notes.md` for the polished format spec and
`rsm-format.txt` for the running RE notebook.

### src/model/ — debugger-agnostic IR

Narrow data structures shared between the input parsers and the
output emitters:

- `Type` variant (Primitive, Pointer, Array, Record, Enum)
- `Symbol` (name, address, size, type id, params, locals)
- `LocalVar` (name, type id, **`stack_offset` = rbp-relative real
  byte offset**, populated by `compose::resolveFunction`; not the
  raw RSM value)
- `LineEntry` (address, file id, line)
- `CompileUnit` (source path, symbols, lines)
- `Module` (units, source files, type pool)

### src/compose/ — Delphi-x64 frame interpreter (single source of truth)

Takes a `rsm::ProcedureRecord` + the function's PE bytes and returns
a `ResolvedFunction { sub_rsp, extra_pushes, vector<ResolvedVar> }`.
Each `ResolvedVar` carries the variable's **rbp-relative real byte
offset**, byte width, optional Pascal `PrimitiveKind`, and flags
(`is_param`, `is_self`, `is_static_link`).

What it implements once, for everyone:

- Prologue decode (`src/pe/prologue.h`): handles both the simple
  `push rbp; sub rsp, imm; mov rbp, rsp` shape and multi-push
  prologues (`push rbp; push rdi; push rsi; sub rsp, imm`). Returns
  `{sub_rsp, extra_pushes}`.
- The frame formula:
  - `real_local = sub_rsp + RSM/2`
  - `real_param = sub_rsp + 8*extra_pushes + RSM/2`
  - `real_self  = sub_rsp + 8*extra_pushes + 16` (Self lives in
    the rcx-shadow slot regardless of the sentinel RSM offset
    attached to markers 0x29 / 0x31 / 0xD5)
- Static-link synthesis: when the procedure record was tagged as
  `has_static_link` (RSM subtag 0x41 = Pascal nested function),
  a `__frame_outer__` slot is prepended pointing at the first
  shadow position (where the parent's rbp gets spilled).
- Three-tier size resolution: Pascal type via the per-unit type
  table (best) > marker-size aggregate from globals > disasm
  sniffer (`pe::sniffStackVarSizes`) of the first memory touch >
  unknown.

Both backends (`src/cli/cli_cmd_pdb.cpp`, `src/cli/cli_cmd_dwarf.cpp`)
consume `ResolvedFunction` directly. No backend re-derives any of
the above. This is enforced by convention -- see CLAUDE.md
"Architecture rules".

### src/dwarf/ — DWARF v5 emitter

Hand-rolled byte emission using a small `BytesBuf` helper with LEB128
and length-field backpatching. Per `CompileUnit`:

- `DW_TAG_compile_unit` with producer / language=Pascal83 / name /
  comp_dir / low_pc / high_pc / stmt_list. **low_pc/high_pc covers
  both line-entry addresses AND function-symbol addresses** so
  compiler-generated unit-init functions don't fall outside the CU
  range. (See docs/03 for why this matters.)
- `DW_TAG_base_type` per primitive used in the CU.
- `DW_TAG_array_type` + `DW_TAG_subrange_type` for byte[N] fallback
  types.
- `DW_TAG_subprogram` per function. If the function has parameters
  or locals, the variant abbrev with `CHILDREN_yes` and
  `DW_AT_frame_base = DW_OP_breg6(0)` (= RBP itself) is used.
  - `DW_TAG_formal_parameter` children, each with
    `DW_OP_fbreg <stack_offset>` where `stack_offset` is the
    rbp-relative real byte offset that `compose::resolveFunction`
    already computed -- no further scaling.
  - `DW_TAG_variable` (typed local) children, same shape.
- `DW_TAG_variable` per global, with `DW_AT_location =
  DW_OP_addr <VA>`. Either typed (with `DW_AT_type` ref_addr) or
  the untyped fallback abbrev.
- Line program in `.debug_line` with explicit `DW_LNS_set_file 0`
  at start (works around the v5 file-register default of 1).

`.debug_str` / `.debug_line_str` are emitted empty; strings are
inlined via `DW_FORM_string`. Simpler, still valid.

Uses LLVM only for the DWARF constants (`llvm::dwarf::DW_*`). No
`llvm::MC` machinery in the production path.

### src/pe/ — PE section injector

Mechanical byte rewriting of the input PE:

- Verifies PE32+ (x64).
- Reserves new section header slots; if `SizeOfHeaders` is too tight,
  expands by one `FileAlignment` unit (shifts all existing section
  data forward by the same amount, updates all `PointerToRawData`
  fields, and `SizeOfImage` / `PointerToSymbolTable`).
- Appends new section raw data past existing data, at FileAlignment
  boundaries.
- Registers each new section in the section header table using the
  COFF "/N" string-table convention for long section names.
- Zeroes the optional-header checksum (gdb / Windows tolerate this).

Does NOT use `llvm::object::COFFObjectFile`. Plain `<windows.h>`
structs plus careful arithmetic.

### src/cli/ — subcommand layer

`main.cpp` is dispatch only (~40 lines). Each `rsm2pdb <verb>` lives
in its own translation unit:

| File | Subcommand |
|---|---|
| `cli_cmd_dump.cpp` | `rsm2pdb dump <input.rsm \| input.map>` |
| `cli_cmd_diff_procs.cpp` | `rsm2pdb diff-procs <map> <rsm>` |
| `cli_cmd_probe_procs.cpp` | `rsm2pdb probe-procs <map> <rsm>` |
| `cli_cmd_analyze_procs.cpp` | `rsm2pdb analyze-procs <map> <rsm>` |
| `cli_cmd_dwarf_emit.cpp` | `rsm2pdb dwarf-emit <map> <out-dir>` |
| `cli_cmd_dwarf.cpp` | `rsm2pdb dwarf <map> <input.exe> <output.exe>` |
| `cli_cmd_pdb.cpp` | `rsm2pdb pdb <map> <exe-in-place> <pdb> [--src-search ...]` |
| `cli_usage.cpp` | usage text |

`cli.h` is the declaration surface (one `int cmd<Name>(...)` per
subcommand, returning a process exit code). Shared helpers in
`cli/util.h` (`extLower`) and `cli/source_path.h` (`resolveSourcePath`).

Adding a subcommand: create the `.cpp`, add its declaration to
`cli.h`, add a dispatch case in `main.cpp`, register the .cpp in
`src/CMakeLists.txt`. Never extend `main.cpp`'s body.

### vscode-ext/ — TypeScript extension

Drives the whole pipeline interactively. See
`docs/05-vscode-extension.md` for the architecture details. Three
modules:

- `dprojParser.ts` — regex-based reader for Delphi MSBuild project
  XML; resolves `(Config, Platform)` tuples through CfgParent
  inheritance.
- `buildRunner.ts` — orchestrates `rsvars.bat` → `msbuild` →
  `rsm2pdb dwarf`, with cancellation (kills process tree via
  `taskkill /T /F`) and a Delphi diagnostic parser that populates
  the PROBLEMS tab.
- `extension.ts` — command registrations + cppdbg launch composition.
  Builds source-search paths via `-environment-directory` per .dproj
  `DCC_UnitSearchPath` / `DCC_IncludePath` / `<DCCReference>` items.

Keybindings: `Ctrl+B` (Make), `Ctrl+Shift+B` (Rebuild), `Ctrl+F9`
(Make + Run), `Ctrl+Shift+F9` (Debug Last), `Ctrl+Shift+C` (Cancel).

## Key design decisions (rationale)

- **Dual input, single model.** `.map` is the authoritative source
  for line tables (machine-generated by the linker, trivial to parse,
  guaranteed consistent with the binary). `.rsm` adds what only it
  has: type definitions, parameter/local locations. Both feed the
  same `model::Module`; the consumer doesn't care.
- **DWARF v5, PDB later.** DWARF is well-documented, has an open
  spec, and is read by gdb / LLDB / IDA Pro / Ghidra. PDB
  (`cppvsdbg`, WinDbg) is on the M3 roadmap.
- **PE injection, not sidecar.** A single debuggable EXE/DLL is
  simpler to deploy and debug. The sidecar alternative (separate
  symbol file loaded via `add-symbol-file`) was considered and
  rejected as more friction for users.
- **Inject at build time.** Modifying a PE post-link triggers
  EDR/Defender "tampered binary" alerts. Building from sources +
  injecting in the same pipeline produces a fresh PE that is
  treated as a normal build output. In-place injection on
  pre-existing DLLs (like Altium AdvPCB.dll) works in practice
  but is environment-dependent.
- **Itanium-style x64 Delphi names are NOT demangled.** Stack traces
  show `Geometry.Add` (the Delphi-source-level qualified name) rather
  than `_ZN8Geometry3AddEii`. Better UX for Delphi developers.
- **`debug.allowBreakpointsEverywhere = true` in VSCode.** cppdbg's
  default whitelist doesn't include Pascal; without this the gutter
  click is silently ignored on `.pas` / `.dpr` files.
- **Function-symbol addresses widen the CU range.** Compiler-
  generated unit-init code falls outside the line-derived address
  range; without absorbing function-symbol addresses too, gdb
  returns "no source" for those addresses. This was a real-world
  bug observed on Altium AdvPCB (see docs/06-gotchas.md #12).
- **Pascal-mode case-insensitive lookup via `-gdb-set case-sensitive
  off`** is set up at session start by the vscode-ext. Without this,
  typing `gd` in the Watch panel fails to find `gD`. gdb's Pascal
  language default is case-sensitive (a long-standing gdb quirk).
