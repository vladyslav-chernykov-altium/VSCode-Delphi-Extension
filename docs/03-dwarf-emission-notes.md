# DWARF emission plan

Target: **DWARF v5**, x86_64, hosted in PE sections of a Windows EXE.

## Sections we emit

| Section name      | Purpose                                |
|-------------------|----------------------------------------|
| `.debug_info`     | DIE tree: CUs, subprograms, types      |
| `.debug_abbrev`   | abbreviation table for `.debug_info`   |
| `.debug_line`     | line-number program                    |
| `.debug_str`      | string pool                            |
| `.debug_line_str` | (v5) file/directory strings            |
| `.debug_aranges`  | address ranges per CU (for fast lookup)|

PE section names use the GNU-style `.debug_*` (NOT `.gnu_debuglink`).
gdb's PE/COFF reader picks them up automatically.

## DIE skeleton (per compile unit)

```
DW_TAG_compile_unit
  DW_AT_producer    "rsm2pdb 0.1"
  DW_AT_language    DW_LANG_Pascal83 (or DW_LANG_ObjC ... see below)
  DW_AT_name        "hello.dpr"
  DW_AT_comp_dir    "C:\Dev\Src\rsm2pdb\examples\01_hello"
  DW_AT_low_pc      <unit-low>
  DW_AT_high_pc     <unit-high>
  DW_AT_stmt_list   <offset into .debug_line>

  + DW_TAG_subprogram   per function   (name, low_pc/high_pc, frame_base)
  +   DW_TAG_formal_parameter ...
  +   DW_TAG_variable          ... (locals)
  + DW_TAG_variable    per global
  + DW_TAG_base_type   per primitive
  + DW_TAG_pointer_type
  + DW_TAG_array_type
  + DW_TAG_structure_type   for records
  +   DW_TAG_member ...
  + DW_TAG_enumeration_type
  +   DW_TAG_enumerator ...
```

### Language code

`DW_LANG_Pascal83` is the registered code for Pascal. gdb knows it but
prints types using Pascal syntax which may surprise users. An alternative
is `DW_LANG_C_plus_plus` which makes gdb apply C++ rules - probably wrong
for Delphi name resolution but more familiar in expression evaluation.

Decision deferred to milestone 2; v1 uses `DW_LANG_Pascal83` and we see
how gdb behaves.

### Calling convention

Delphi `register` (default) is **not** standard System V or Microsoft x64.
Param locations:
- First three integer/pointer params: RCX, RDX, R8 (x64) - matches MS x64.
- Floats use XMM0..XMM3 - matches MS x64.
- Self for methods: RCX - matches MS x64 `this`.

So for 64-bit Delphi the default `register` convention is in practice the
**Microsoft x64 calling convention**. That means DW_AT_calling_convention
= `DW_CC_normal` is fine, and DW_AT_location for parameters uses the
standard MS x64 register set.

(Verify on first real sample. If we see params spilled to home space
mid-prolog, we need DW_AT_location list expressions, not single regs.)

## CU address range (`low_pc` / `high_pc`)

Each compile unit's `[low_pc, high_pc)` is computed from BOTH:

1. The MIN/MAX address of its **line entries** (covers all executable
   statements that Delphi mapped to source lines).
2. The MIN/MAX address of its **function symbols** (covers whole
   functions including compiler-generated ones like the unit-init
   `<Unit>.<Unit>` for the `.dpr`'s `begin..end` block).

**Why both:** gdb consults a CU's line program only when an address
falls inside that CU's `[low_pc, high_pc)`. If we derived the range
from line entries alone, addresses inside compiler-generated functions
(which may have sparse / no line entries) would fall outside any CU's
range, and gdb would report "no source" for breakpoints there — even
when the line table contained valid nearby entries.

This was the symptom that map2pdb on the same `.map` did not exhibit;
it emits per-function ranges in CodeView, so every function's address
maps to a module entry. We get the equivalent in DWARF by widening
the CU range.

Implemented in `src/dwarf/dwarf_emitter.cpp cuRange()`. The function
walks both `cu.lines` and `cu.symbols` (kind=Function) and absorbs
their addresses into a single `[lo, hi]` pair.

Remaining gap: bare `begin`/`end` lines without any executable
statement still lack line-table entries — Delphi doesn't emit them.
Breakpoints on such lines hit but don't navigate to source. Workaround
is to set the breakpoint on any actual statement; future work is to
synthesise line entries at function boundaries to match map2pdb.

## Line table

DWARF's line state machine matches RSM's line table conceptually. We
build a sequence per function:
```
DW_LNS_set_file <file_id>
DW_LNS_advance_pc <delta>
DW_LNS_advance_line <delta>
DW_LNS_copy
...
DW_LNE_end_sequence
```

LLVM's `MCDwarfLineTable` builder takes care of state-machine encoding;
we feed it `(file, line, address)` tuples.

## Type mapping (current scope: M2 phase B-lite)

The emitter currently handles `DW_TAG_base_type`, `DW_TAG_array_type`,
and `DW_TAG_subrange_type`. `DW_TAG_pointer_type`, `DW_TAG_structure_type`
and `DW_TAG_enumeration_type` are not yet emitted.

| Delphi             | DWARF                                          |
|--------------------|------------------------------------------------|
| Boolean            | `DW_TAG_base_type` enc=`DW_ATE_boolean` size=1 |
| AnsiChar           | `DW_TAG_base_type` enc=`DW_ATE_unsigned_char` size=1 |
| Char / WideChar    | `DW_TAG_base_type` enc=`DW_ATE_UTF` size=2     |
| Byte / ShortInt    | `DW_TAG_base_type` enc=`DW_ATE_unsigned/signed` size=1 |
| Word / SmallInt    | ditto, size=2                                  |
| Cardinal / Integer | ditto, size=4                                  |
| UInt64 / Int64     | ditto, size=8                                  |
| Single             | `DW_ATE_float` size=4                          |
| Double             | `DW_ATE_float` size=8                          |
| Extended           | `DW_ATE_float` size=10                         |
| Static array       | `DW_TAG_array_type` + `DW_TAG_subrange_type`   |
| Record             | (B-lite) fallback `byte[N]` array              |
| Enum               | (B-lite) fallback `byte[N]` array              |
| Pointer            | (B-lite) fallback `byte[8]` array              |

### Variable typing (M2 phase B-lite)

For global variables we cross-reference the `.map` (segment + address)
against the RSM's variable-record stream. The RSM tells us whether a
global is primitive-typed or non-primitive (records/enums/classes);
the primitive form additionally carries a *per-unit type marker* that
groups variables of the same Pascal type together.

The exact Pascal primitive is inferred from the variable's byte size,
computed as the next-symbol-gap in the `.map`:

| Byte size | Inferred type |
|----------:|---------------|
| 1         | `Byte`        |
| 2         | `Word`        |
| 4         | `Integer`     |
| 8         | `Int64`       |
| other     | `byte[N]`     |

The heuristic loses signed/unsigned and float-vs-integer distinctions
at the same size (Cardinal shown as Integer, Single shown as Int64,
etc.) — accepted per D-017. Sub-byte primitives (`Byte`, `ShortInt`,
`Boolean`) padded for alignment in the `.map` show as the padded size
(`Word` or wider). The bytes are always correct; only the displayed
type name and unit may be inflated by alignment.

### Subrange bounds

gdb's Pascal mode treats `DW_AT_lower_bound` as 1 by default. To make
`byte[N]` cover all N elements as the natural `array [1..N] of Byte`,
the emitter writes `DW_AT_upper_bound = N` (not N − 1).

Deferred (later milestones): sets, variants, dynamic arrays,
ShortString, AnsiString, UnicodeString, interfaces, classes, pointer
types, precise record/enum decoding.

### Workarounds for type-display ambiguity (gdb)

Because the size-based heuristic merges same-size primitives, end
users hit display mismatches like `gD: Double` showing as
`Int64 = 4612811918334230528` (the IEEE-754 bytes of 2.5
reinterpreted as a 64-bit integer). Three workarounds, all verified:

**1. Debug Console, gdb format specifiers.** Works in either
language mode without any setup:

```
-exec print/f gD          # 2.5  (8-byte float view; only useful
                          #       when our DWARF type is also 8 bytes)
-exec print/u gC          # 2    (unsigned view of a Cardinal we
                          #       mis-typed as Integer)
-exec print/x gD          # 0x4004000000000000  (raw bytes)
```

`/f` won't help for `Single` (we emit it as Int64 = 8 bytes, so `/f`
treats all 8 bytes as `double` and returns nonsense). Use
workaround 2 for `Single`.

**2. VSCode Watch panel, C-style reinterpret cast.** gdb's Pascal
expression evaluator rejects C pointer-cast syntax with "syntax
error in expression". Switch the evaluator language once per
session via Debug Console:

```
-exec set language c
```

Then in Watch:

```
*(double*)&gD   ⇒ 2.5
*(float*)&gF    ⇒ 1.5
(unsigned)gC    ⇒ 2
```

gdb prints a one-line "language does not match frame" warning
on switch — cosmetic, can be ignored. Watch entries update live
as you step.

**3. (Doesn't work) `gD,f` comma-format suffix.** That's a
Visual Studio cppvsdbg feature; the gdb-backed cppdbg adapter does
not translate it and passes `gD,f` to gdb as a C expression (the
comma operator), which evaluates to `f` and fails as an undefined
symbol.

### Why Pascal-mode Watch can't do reinterpret casts

gdb's Pascal expression evaluator only recognises types that
appear in DWARF DIEs for the current CU. Our emitter doesn't emit
`Double`, `Single`, `Cardinal`, or `UInt64` for our fixtures
(those primitives are merged into `Int64` / `Integer` by the
size heuristic), so a Watch expression like `Double(@gD)^` fails
with "No symbol Double in current context".

Enabling Pascal-style casts would require always emitting a
baseline set of primitive DIEs even when no variable uses them
(~20 lines of emitter change), then verifying that gdb's Pascal
grammar accepts the `PType(@v)^` reinterpret form. Untested;
deferred until/unless the typing heuristic is replaced by the
precise bridge (step 10.5).

## LLVM APIs to use

- `llvm::MCContext`             - shared MC context
- `llvm::MCObjectFileInfo`      - sets section conventions
- `llvm::MCStreamer`            - object streamer (we use object-file streamer)
- `llvm::MCDwarfLineTable`      - line-number emission
- `llvm::DIE` / `llvm::DIEAbbrev` - DIE construction
- `llvm::DwarfStringPool`       - string pool for `.debug_str`

For the actual byte output we write into raw buffers and then append
them as new sections to the PE (custom COFF section emitter using
`llvm::object::COFFObjectFile` for parse + a small writer for append).

## Injection mechanics (PE)

1. Parse input PE: section table, headers, file alignment, section
   alignment.
2. Build new section headers for each `.debug_*` we have data for.
3. Append each section's raw data to the end of the file at the next
   file-alignment boundary.
4. Patch the PE: bump `NumberOfSections`, extend `SizeOfImage`,
   recalculate checksum (or zero it - WinDbg/gdb don't enforce).
5. Make sure new sections' `Characteristics` = `IMAGE_SCN_MEM_READ |
   IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_DISCARDABLE`.

The size of the section header table is bounded; if the PE has no room
between the last header and the first section's raw data, we move
section data to make room. Most Delphi EXEs have headroom because the
linker reserves 0x400 bytes for headers and uses < 0x400.
