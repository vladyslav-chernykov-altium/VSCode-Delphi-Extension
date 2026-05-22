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

## Type mapping (v1 scope)

| Delphi          | DWARF                                          |
|-----------------|------------------------------------------------|
| Boolean         | `DW_TAG_base_type` enc=`DW_ATE_boolean` size=1 |
| Char            | `DW_TAG_base_type` enc=`DW_ATE_UTF` size=1     |
| WideChar        | `DW_TAG_base_type` enc=`DW_ATE_UTF` size=2     |
| Byte / ShortInt | `DW_TAG_base_type` enc=`DW_ATE_unsigned/signed`|
| Word / SmallInt | ditto, size=2                                  |
| Cardinal / Integer | ditto, size=4                               |
| UInt64 / Int64  | ditto, size=8                                  |
| Single          | `DW_ATE_float` size=4                          |
| Double          | `DW_ATE_float` size=8                          |
| Extended        | `DW_ATE_float` size=10 (only on win32; check)  |
| Pointer         | `DW_TAG_pointer_type`                          |
| Static array    | `DW_TAG_array_type` + `DW_TAG_subrange_type`   |
| Record          | `DW_TAG_structure_type` + members              |
| Enum            | `DW_TAG_enumeration_type` + enumerators        |

Deferred to v2: sets, variants, dynamic arrays, ShortString, AnsiString,
UnicodeString, interfaces, classes.

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
