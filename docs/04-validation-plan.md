# Validation plan

How we know each milestone works.

## M1 — Source-level navigation, no variables  (DONE)

**What ships in M1:**

- Breakpoint by source:line in `.pas` / `.dpr` files
- Step over / into / out
- Call Stack with source file, line number, and clean Delphi
  function name (`Geometry.Add at Geometry.pas:20`)
- `break Geometry.Add` works by qualified name

**Verification gates:**

1. **Unit tests** in `test/unit/` (currently 16 cases / 137 assertions
   all green). Coverage:
   - `.map` parser at the structural level (synthetic snippets) and
     against real fixtures (`hello.map`, `two_units.map`).
   - `MapFile -> model::Module` adapter: address absolutisation,
     prefix-keeping for symbol names, aux-symbol filtering.
   - DWARF emitter byte layout: CU header field positions, abbrev
     table structure, line program contains the right address bytes
     and `DW_LNS_set_file 0` at program start.
   - Multi-CU DIE emission.

2. **Integration: `rsm2pdb dwarf` round-trip on a real fixture**
   (`examples/02_two_units/build.cmd`):
   - Delphi MSBuild produces `.exe` + `.rsm` + `.map`.
   - `rsm2pdb dwarf` injects DWARF; the resulting exe runs and
     prints expected output (`S=4 D=25 C=green`).

3. **gdb-driven assertions** (manual today; integration harness
   planned):
   - `gdb --batch -ex 'info sources' <exe>` lists all user `.pas`/
     `.dpr` files.
   - `gdb --batch -ex 'info functions Geometry'` lists user
     subprograms with their source file + line.
   - `gdb --batch -ex 'break Geometry.Add' -ex 'run' -ex 'bt'
     -ex 'continue' <exe>` hits the breakpoint, shows a stack trace
     with source file + line numbers + clean Pascal function names,
     and the program completes normally.

4. **VSCode end-user verification** (user-confirmed 2026-05-22):
   - Gutter click on `.pas`/`.dpr` sets a breakpoint.
   - F5 launches via the included cppdbg launch config.
   - Breakpoint fires; F10 / F11 step navigation works.
   - Call Stack panel shows Delphi function names + source:line.

## M2 — Variable inspection  (planned)

**What M2 will ship:**

- Variables panel shows locals, parameters, globals with their
  values.
- Records render with field names: `P: TPoint = {X=3, Y=4}`.
- Enums render with enumerator names: `C: TColor = clGreen`.
- Hover-to-evaluate over a variable name in source.

**Required pieces:**

- `src/rsm/` parser: type records (primitive, pointer, array, record,
  enum), parameter/local locations from `.rsm`.
- DWARF emitter D3: `DW_TAG_structure_type`, `DW_TAG_member`,
  `DW_TAG_enumeration_type`, `DW_TAG_enumerator`,
  `DW_TAG_variable`, `DW_TAG_formal_parameter` with
  `DW_AT_location` expressions.
- New fixtures exercising records, enums, function parameters.

**Verification gates (planned):**

- Unit tests for RSM record decoders (synthetic byte snippets).
- Fixture round-trip: RSM bytes → model::Type tree → DWARF →
  gdb shows correct rendering.
- `gdb --batch -ex 'break Geometry.Add' -ex 'run' -ex 'print A'`
  yields `$1 = 3`.

## Reference debuggers

In addition to gdb, the produced DWARF can be sanity-checked against:

- **LLDB**: `lldb hello_dbg.exe ; image lookup -s Geometry.Add`.
- **IDA Pro / Ghidra**: open the binary, verify DWARF is recognised
  and function names appear in the listing.

These are not gating but flag emission bugs that gdb happens to
tolerate.

## Future (M3)

- **PDB output backend** validated with Microsoft `cvdump.exe`,
  WinDbg `lm v m <module>` + `x <module>!*`, and VSCode `cppvsdbg`.
