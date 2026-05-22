# Architecture

```
.pas / .dpr ─┐
              │  Delphi MSBuild
              ▼
       hello.exe + hello.rsm + hello.map
              │
              ▼
   ┌─────────────────────┐    ┌─────────────────────┐
   │  src/map/           │    │  src/rsm/           │
   │  text parser        │    │  binary parser      │
   │  (M1 input source)  │    │  (M2; stub today)   │
   └──────────┬──────────┘    └──────────┬──────────┘
              │                          │
              │     populate()           │  (later)
              ▼                          ▼
                  ┌─────────────────────┐
                  │   model::Module     │
                  │ (debugger-agnostic) │
                  │ source_files,       │
                  │ CompileUnits,       │
                  │ Symbols,            │
                  │ LineEntry           │
                  │ Type (future)       │
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
                  │ - expands           │
                  │   SizeOfHeaders     │
                  │ - COFF /N string    │
                  │   table for long    │
                  │   section names     │
                  └──────────┬──────────┘
                             │
                             ▼
              hello_dbg.exe  (DWARF-enriched PE)
                             │
                             ▼
                       gdb (mingw-w64)
                             │
                             ▼
                     VSCode cppdbg adapter
```

## Layer responsibilities

### src/map/ — Delphi .map parser

Parses Delphi's detailed map file (DCC_MapFile=3). Section-aware
state machine recognising:

- Segments overview (`.text` at 0x401000, `.data`, `.bss`, ...)
- Detailed map of segments (per-module address ranges)
- Publics by Name / by Value (2270 entries in our hello fixture)
- Line numbers for `<unit>(<source>) segment <name>` (one block per
  user unit)
- Program entry point

Adapter `populate(MapFile, model::Module)` converts the raw map view
into the debugger-agnostic IR. It:

- Computes absolute VAs from segment-relative offsets.
- Emits one `model::CompileUnit` per source file the map advertises.
- Keeps Delphi-qualified names (`Geometry.Add`, not bare `Add`) so
  stack traces and `break X.Y` work as users expect.
- Filters out linker-generated EH aux symbols (`$pdata$`, `$unwind$`)
  and anonymous compiler-internal symbols.

### src/rsm/ — Delphi .rsm parser  (M2)

Currently a stub. The plan is to parse the modern Embarcadero
"CSH7"-magic RSM format and feed type info, parameter/local locations,
and additional symbols into the same `model::Module`. See
`rsm-format.txt` for the running reverse-engineering notebook.

### src/model/ — debugger-agnostic IR

Narrow data structures shared between the input parsers and the
output emitters:

- `Type` variant (primitive, pointer, array, record, enum)
- `Symbol` (name, address, size, type id)
- `LineEntry` (address, file id, line)
- `CompileUnit` (source path, symbols, lines)
- `Module` (units, source files, type pool)

### src/dwarf/ — DWARF v5 emitter

Hand-rolled byte emission using a small `BytesBuf` helper with LEB128
and length-field backpatching. Currently emits, per `CompileUnit`:

- One `DW_TAG_compile_unit` DIE with producer / language=Pascal83 /
  name / comp_dir / low_pc / high_pc / stmt_list.
- One `DW_TAG_subprogram` child per public function with name /
  low_pc / high_pc / external.
- A line program in `.debug_line` (set_address + advance_pc/line +
  copy + end_sequence; no special opcodes yet). `DW_LNS_set_file 0`
  is emitted at program start to work around the v5 file-register
  default of 1.

Skipped intentionally:

- `.debug_str` / `.debug_line_str` are emitted empty; strings are
  inlined via `DW_FORM_string` instead. Simpler, still valid.
- `.debug_aranges`, `.debug_frame`, `.debug_loclists`,
  `.debug_rnglists` — not required for source:line + symbol
  inspection in gdb.

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
  COFF "/N" string-table convention for long section names (Delphi
  PEs typically have no symbol table; we create a minimal one).
- Zeroes the optional-header checksum (gdb / Windows tolerate this).

Does NOT use `llvm::object::COFFObjectFile`. Plain `<windows.h>`
structs plus careful arithmetic — easier to reason about and to
verify byte-by-byte against the PE/COFF spec.

## Key design decisions (rationale)

- **Dual input, single model.** `.map` is the authoritative source
  for line tables (machine-generated by the linker, trivial to parse,
  guaranteed consistent with the binary). `.rsm` adds what only it
  has: type definitions, parameter/local locations, name-mangling
  context. Both feed the same `model::Module`; the consumer doesn't
  care.
- **DWARF v5, no PDB yet.** DWARF is well-documented, has an open
  spec, and is read by gdb / LLDB / IDA Pro / Ghidra. PDB
  (`cppvsdbg`, WinDbg) is on the M3 roadmap.
- **PE injection, not sidecar.** A single debuggable EXE is simpler
  to deploy and debug. The sidecar alternative (separate symbol file
  loaded via `add-symbol-file`) was considered and rejected as more
  friction for users.
- **Inject at build time.** Modifying a PE post-link triggers
  EDR/Defender "tampered binary" alerts. Building from sources +
  injecting in the same pipeline produces a fresh `.exe` that is
  treated as a normal build output.
- **Itanium-style x64 Delphi names are NOT demangled.** Stack traces
  show `Geometry.Add` (the Delphi-source-level qualified name) rather
  than `_ZN8Geometry3AddEii`. Better UX for Delphi developers.
- **`debug.allowBreakpointsEverywhere = true` in VSCode.** cppdbg's
  default whitelist doesn't include Pascal; without this the gutter
  click is silently ignored on `.pas` / `.dpr` files.
