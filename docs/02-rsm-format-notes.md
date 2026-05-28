# RSM format notes

Embarcadero's `.rsm` (Remote Symbol) file is the debug-info container
that ships alongside Win64 EXEs when Delphi is built with debug info on
and Win64 platform. Generated automatically by the Delphi linker for
Win64 targets.

The format is **not** the legacy Borland TDS/TD32 layout. The magic is
ASCII `"CSH7"`, not `"FB09"`/`"FB0A"`. The Embarcadero RSM format is a
redesign, not a repackaging. JCL's `JclTD32.pas` is therefore an
intellectual-history reference, not a translation source. We reverse-
engineer it from scratch using `.map` files as a Rosetta stone.

The messy lab notebook with raw hex evidence and dated entries lives
in [../rsm-format.txt](../rsm-format.txt). This document is the
polished current-best-understanding.

## Reference material (limited usefulness)

- **JCL (Jedi Code Library)** — `JclTD32.pas`, `JclDebug.pas`,
  `JclPeImage.pas`. Reads the legacy TD32 layout; record-kind ideas
  may carry over but byte layouts do not.
- Borland TASM / TD32 spec leaks (1990s) and Microsoft CodeView (CV4 /
  CV5) docs. Same caveat.

## Fixtures

| Path | Source | Size | Built |
|---|---|---:|---|
| `test/fixtures/hello.rsm`      | single-file `hello.dpr`                          | 3,516,365 | 2026-05-22 |
| `test/fixtures/two_units.rsm`  | `Geometry` + `App.Colors` + dpr                  | 3,519,722 | 2026-05-22 |
| `test/fixtures/primitives.rsm` | 13 user globals across 12 primitive types        | 3,514,463 | 2026-05-22 |

Both produced by Delphi 10.2 Tokyo (`v32.0`), Win64, debug build,
`DCC_DebugInfoInExe=false`. Each has its companion `.exe` and `.map`.

## Header — bytes 0x00..0x1F

Verified by byte-diffing both fixtures and cross-referencing the
`.map`. All fields are little-endian uint32.

| Offset | Field          | hello | two_units | Notes |
|-------:|----------------|-------|-----------|-------|
| 0x00 | `magic`          | `"CSH7"` | `"CSH7"` | ASCII; format constant. |
| 0x04 | `metadata_start` | `0x00000420` | `0x00000420` | Format constant. Metadata stream itself begins at 0x426 (6-byte gap may be a small sub-header). |
| 0x08 | `unit_count`     | **14** | **16** | Number of linked compilation units. Matches distinct `M=` modules in the .map's "Detailed map of segments". |
| 0x0C | `version_minor?` | `1` | `1` | Always 1 in our samples. Possibly format-version sub-field. |
| 0x10 | `timestamp?`     | `0x5CB65217` | `0x5CB656A4` | Unix timestamps in April 2019. *Not* the .rsm's own build time (both .rsm built May 2026). Hypothesis: some Delphi 10.2 compiler-build stamp. Low priority. |
| 0x14 | `flags?`         | `0x2000234D` | `0x2000234D` | Format constant. |
| 0x18 | `legacy_imagebase` | `0x00400000` | `0x00400000` | Format constant. Not the actual Win64 ImageBase; legacy 32-bit default carried over. |
| 0x1C | padding          | `0` | `0` | |

Past 0x1F: null-terminated ASCII path to the EXE this RSM describes
(e.g. `.\Win64\Debug\hello.exe`), relative to the .dproj directory.
The remainder of the 0x40..0x425 range is zero padding.

## Metadata stream — starting at 0x426

A `0x426` byte-position is constant across both fixtures. Contents
known so far:

- **0x426..~0xAD0:** library / unit / hpp / include / object / resource
  search-paths block. The 6 standard Delphi search-path categories,
  each emitted as a null-terminated path list, repeated for a small
  fixed count (count byte `0x07 0x00` at 0x426). Bytes identical
  between fixtures because both used the same Delphi install.
- **~0xADE:** `DCC_UnitAliases` value (one big semicolon-delimited
  string).
- **~0xB90:** the EXE path string reappears (also present at 0x20).
- **~0xBBF onward:** Delphi RTL unit names (`System`, `SysUtils`,
  `Winapi.Windows`, …) and their imported symbols
  (`@DelayLoadHelper2`, `kernel32.dll`, `VirtualAlloc`, …). Comprises
  most of the file's 3.5 MB.

## User-code records — near end of file (~0x355000+)

User globals, functions, and types are encoded near the file tail.
Records open with a one-byte **kind tag**, followed by a length-
prefixed name and a kind-specific payload.

Confirmed kind tags:

| Tag | Kind | Notes |
|-----|------|-------|
| `0x20` | Variable | global variable record (the one we care about for M2-B-lite). |
| `0x27` | Function aux | `$unwind$_ZN...` / `$pdata$_ZN...` SEH metadata. Skip. |
| `0x28` | Function / unit | user function names like `Finalization`, `two_units`. |
| `0x2A` | Primitive type | one record per built-in Delphi type. See below. |
| `0x03` | Enum type | followed by enum-name then length-prefixed enumerator strings. |

### Primitive-type record (tag `0x2A`)

The full set of built-in Delphi types is enumerated near the start of
the metadata stream — in our fixtures, records run from ~0x147F to
~0x19BB (28 entries: `Boolean`, `AnsiChar`, `Char`, `WideChar`,
`ShortInt`, `SmallInt`, `Integer`, `Byte`, `Word`, `Cardinal`,
`Pointer`, `LongInt`, `LongWord`, `Int64`, `UInt64`, `NativeInt`,
`NativeUInt`, `Single`, `Real48`, `Extended80`, `Extended`, `Double`,
`Real`, `Comp`, `Currency`, `ByteBool`, `WordBool`, `LongBool`).

Layout:

```
0x2A  <namelen u8>  <pascal_name>  <subtag triple>
      <hash u32?>   <attribute bytes>
      0x9C 0x13  <type_id u16-LE>  <cpp_name_block>
      0xFF
```

- The `0xFF` terminator also appears inside record payloads, so the
  reliable way to find the record end is to scan forward for the next
  `0x2A <printable namelen> <ASCII letter>` and take the byte just
  before that as the terminator.
- The 3-byte sub-tag right after the name takes one of three values
  (`0xA8 00 00`, `0x88 00 00`, `0x80 00 00`); meaning still unclear.
- The 2-byte type-id following `0x9C 0x13` is **stable across builds**
  (verified by comparing hello.rsm and two_units.rsm) and **unique per
  primitive**. Type-ids increment by 2 in a contiguous block (0x08E4
  for `Boolean` through 0x08FE for `Int64`), then jump to 0x0201 and
  continue with mostly-+4 increments.

Byte size and signedness are not independently stored in the record
(or at least not in any field we've identified) — the parser maps the
Pascal name to `(byte_size, model::PrimitiveKind)` via a hardcoded
table. Delphi's primitive set is closed and well-known, so this is a
safe approach.

The 4-byte field immediately after the subtag triple looks like a
hash or RTTI signature (unique per primitive); not currently used.

### Variable record (tag `0x20`)

```
0x20  <namelen u8>  <name bytes>  0x66  0x00  0x00  <payload>
```

Payload comes in **three** variants:

| Form | Bytes | Layout |
|---|---:|---|
| Primitive, plain | **5** | `<type_marker u8> <shifted_va u32>` |
| Primitive, extended | **12** | `<type_marker u8> <shifted_va u32> 0x9C 0x09 <hash u16> <trailer_type_id u16> 0xFF` |
| Non-primitive | **6** | `<inline_type_id u16-LE> <shifted_va u32>` |

The primitive plain form is used for RTL globals and the last user
variable in a unit; the extended form is used for most user-code
globals. The two are discriminated by looking for the `0x9C 0x09`
trailer marker at payload[5..6]. The non-primitive form is selected
when the first two bytes look like a valid type-id rather than a
small even type marker (markers are 0x02, 0x04, 0x06, …).

There is no record-terminator byte; the next record follows
immediately after the variable's fixed-size payload.

**VA encoding.** The 4-byte address-like field is stored as a little-
endian uint32 **left-shifted by 4 bits** relative to the actual VA:

```
actual_va = stored_u32 >> 4
```

Verified against all user globals in both fixtures.

**Type binding (non-primitive).** The `type_id u16` indexes into the
RSM type table. Two `TPoint` globals (P1, P2 in two_units) share the
same type_id; the `TColor` global gets a different one. Across units,
identical structural types get different ids — e.g. the inline
`TPoint` declared in `hello.dpr` (0x1A71) is a separate id from the
`TPoint` imported from `System.Types` in `two_units.dpr` (0x064D).

**Type binding (primitive).** The byte right after the `0x66 0x00 0x00`
sub-tag is a **per-unit type marker** — variables of the same Pascal
primitive type within one compilation unit share the same marker, and
markers are assigned in declaration order in increments of 2 (Integer
declared first → marker `0x02`; next new type → `0x04`; and so on).
The 7-byte trailer's `trailer_type_id u16` field mirrors the same
information with a +4 step.

Markers are **not globally stable**: in a unit where `Byte` is declared
before `Integer`, `Byte` would get marker `0x02`. To resolve a marker
to a concrete Pascal type, the consumer needs to combine it with the
size derived from the `.map` next-symbol gap (1 byte → `Byte`, 2 →
`Word`, 4 → `Integer`, 8 → `Int64`). Variables sharing a marker share
a type, so even one sized example resolves all others.

### Procedure record (tag `0x28`)

```
0x28  <namelen u8>  <pascal_name>
      <fn_subtag u8>  0x00  0x00          (subtag ∈ {0xA0, 0xE0, 0x80})
      <hash u32>  <shifted_va u32>        (VA decodes as stored >> 4)
      <variable-length trailer 3..~30 bytes>
      <sub-records ...>                   (zero or more 0x21/0x20)
      0x63                                 (record-end marker)
```

The trailer between the VA and the first sub-record encodes return-
type / calling-convention / attribute info that we don't yet decode.
**Its length is not constant** — empirically 3 to ~30 bytes, with 5–8
covering the bulk:

| Trailer bytes | Share on Altium AdvPCB |
|---:|---:|
| 7 | 27 % |
| 6 | 26 % |
| 5 | 16 % |
| 8 | 15 % |
| 9 | 5 % |
| 10 | 1.5 % |
| 3, 4, 11..30 | ~9 % combined |

The proper anchor for finding the first sub-record is therefore a
bounded **scan-forward** from `name_end + 11` looking for a valid
sub-record header or a real end marker.

Sub-records are tag-prefixed; the param/local list uses two tags:

| Tag | Kind |
|---|---|
| `0x21` | formal parameter |
| `0x20` | local variable (includes `Result` for functions) |

Payload after the name is a **fixed 5 bytes**:

```
<tag>  <namelen u8>  <pascal_name>  <subtag u8>  0x00  0x00  <type_marker u8>  <stack_offset i8>
```

**Sub-record subtag is variable.** Of 215k decoded sub-records on
AdvPCB:

| Subtag | Share | Subtag | Share |
|---|---:|---|---:|
| `0x66` | 56.4 % | `0x12` | 1.9 % |
| `0x16` | 26.0 % | `0x22` | 1.3 % |
| `0x26` |  8.3 % | `0x06` | 0.6 % |
| `0x62` |  5.0 % | tail   | 0.5 % |

The strong invariant is the **two zero bytes** at offsets `+1`,`+2`
after the subtag — that's the signature to anchor on. The subtag
byte itself is informational (probably type-class / by-reference /
register-passed flags); a parser that wants robustness should accept
any byte there and trust the zero-zero invariant.

After the param/local list, some procs carry additional sub-records
with tags **`0x23`** (return-value descriptor) and **`0x25`** (enum
entries for inline-declared enum return types). These don't fit the
5-byte sub-record format. The pragmatic rule is to stop the param/
local walk on any tag other than `0x20`/`0x21`/`0x63` and keep
whatever has been collected.

Between consecutive sub-records the encoding can carry an extra 1–2
byte attribute prefix (likely `var` / `out` / `const` qualifiers).
Robust parsing scans 0..6 bytes forward from the expected `bodyAt + 5`
for the next valid sub-record header before giving up.

**Stack-offset encoding.** RSM stores 1-byte signed offsets in 2-byte
stride units relative to a frame anchor at `rbp + 16`:

```
real_byte_offset = (rsm_offset_signed / 2) + 16
```

In Delphi Win64 the actual spill / local area starts at `rbp + 32`
(not the MS-x64 `rbp + 16`). This formula was verified against
Geometry.Add and DistanceSq on the small fixture and against ~125k
matched procs on Altium AdvPCB.

**Open: override / virtual methods' `Self`.** For override methods,
`Self` is stored in the RSM with `stack_offset = +4`, which decodes to
`rbp + 18` — misaligned and clearly not the real Self location. For
non-override methods, `Self` typically has `stack_offset = +32`
(`rbp + 48`). The `+4` value is likely a sentinel ("`Self` is in RCX
/ at `rbp + 8`, not spilled") rather than a literal offset. The
proc-record's variable-length trailer plausibly carries the
virtual/override discriminator. Workaround: locals and non-Self
params for override methods are correct; only the displayed value of
`Self` itself is wrong.

### Scanner caveats

The same `0x20 ... 0x66 0x00 0x00` byte pattern occurs inside
procedure-local-variable records (different semantic — frame offsets
rather than VAs), and in unrelated RTL data. Our scanner therefore
finds ~800 candidates in each fixture; only ~40 land in the plausible
PE-image VA range. The bogus ones drop out cleanly when we cross-
reference against the `.map` segment table.

### Per-unit type table (the marker → Pascal name bridge)

Each Pascal unit's metadata block contains a **primary type table**
that maps `Variable::type_marker` (a per-unit even byte, `0x02 /
0x04 / 0x06 / ...`) to a Pascal type name. The table is a sequence
of records in source-declaration order:

```
0x66 <namelen u8> <pascal name> <4-byte hash>
```

`marker = 2 * (1-based index)`, so the first 0x66 record is marker
0x02, the second 0x04, etc.

Records are **interleaved with `0x67` entries** of the same shape
that reference imported functions / methods — those are skipped
during scanning, they don't get a marker assigned. Example from
`examples/04_locals` "locals" unit:

```
66 07 "Integer"       <- marker 0x02
67 07 "Writeln"       <- skip (function reference)
66 06 "string"        <- marker 0x04
66 0d "UnicodeString" <- marker 0x06
67 06 "Output..."     <- skip
```

To locate each unit's primary table we look for the **unit anchor**
signature: `0x02 <namelen> <unit name>` followed (within ~200 bytes)
by `0x02 0x70 <namelen> <file>.{pas|dpr|inc|tmp}`. The first 0x66
record after the anchor begins the unit's primary type table; the
scan terminates at the next anchor or after a generous upper bound.
This anchor-bound search avoids cross-unit marker collisions which
the older "consecutive 0x66 run" heuristic suffered from (every
marker 0x02 in 04_locals used to resolve to "Boolean" because the
"locals" anchor-less candidate was discarded and the lookup landed
in an unrelated downstream table).

Some Delphi RTL units use layout variants that don't match the
anchor signature. Variables in those units stay with empty
`pascal_type` and fall back to size-based UInt8/16/32/64 hex in the
PDB writer — same behaviour as before any type resolution, no
regression.

Lookup `pascal_type` in the built-in `kPrimitiveTable`
(in `src/rsm/rsm_reader.cpp`) to recover `(PrimitiveKind,
byte_size)` — `Reader::resolvePrimitive(name)` is the public entry
point. From there `pdb_writer.cpp::typeForKindParts` picks the
matching CodeView `SimpleTypeKind` (Int32 / UInt32 / Real32 /
Real64 / Boolean8 / WideCharacter / NarrowCharacter / ...). String
types (UnicodeString / AnsiString / WideString / UTF8String / PChar
family) are emitted with `SimpleTypeMode::NearPointer64` over a
char base type, which makes cdb auto-display them as `"..."`.

### Sub-record stack-offset encoding

Procedure sub-records (tags `0x20` / `0x21` / `0x22` for param,
local, var-param respectively) carry the stack offset in one of
two compact encodings:

| form | byte[0] LSB | size | decode |
|---|---|---|---|
| 1-byte | **0** | 1 byte | `stored = (int8) b0` |
| 2-byte | **1** | 2 bytes (LE word `v`) | `stored = ((int16) v - 1) / 2` |

The 2-byte form is **signed** (i16, not u16). It's used when the
offset doesn't fit a signed i8 — typically for string / bool /
char locals in large procedures whose offsets land in the
-200..-1600 byte range. Decoding it as unsigned puts the
variable at a stack address well past the function's frame, which
is what bit early-session PDB output for `ProbeLocals`'s strings
("NULL pointer" display in cdb).

Both forms normalise to `stored_off / 2` units so the downstream
real-offset formula stays uniform:

```
real_local  = sub_rsp + stored_off / 2
real_param  = sub_rsp + 8*extra_pushes + stored_off / 2
real_self   = sub_rsp + 8*extra_pushes + 16
```

`sub_rsp` and `extra_pushes` come from disassembling the function's
prologue (see `src/pe/prologue.h`). `extra_pushes` counts callee-
saved `push <reg>` instructions between `push rbp` and `sub rsp`;
each one shifts the rcx-shadow / param-spill area up by 8 bytes.
Locals (which sit at `rbp + 0 .. rbp + sub_rsp`) are not affected
by extra pushes.

## What we still don't know

- **VA encoding inside variable-record payloads.** The four
  "address-like" bytes don't decode as a plain little-endian uint32
  matching the .map VA — they're stored as a value shifted *left* by
  4 bits (i.e. `actual_va = stored_u32 >> 4`). Confirmed against
  multiple globals; will be wired into the variable-record parser
  in the next milestone step.
- **Variable → primitive type-id bridging.** ~~Variable records carry
  a `0x9C 0x09 ?? ??` reference, distinct from the primitive table's
  `0x9C 0x13 ?? ??` marker.~~ **Resolved 2026-05-27.** Variables
  carry a small `type_marker` (even byte, `0x02 / 0x04 / 0x06 / ...`)
  that indexes into the unit's primary type table — see "Per-unit
  type table" above. The `0x9C 0x09 <hash> <trailer_type_id>`
  trailer turned out to be a per-variable slot ID (not a type
  reference); for resolution we only need the marker. RSM primitive
  table's `raw_type_id` is global but doesn't need to be touched —
  joining `marker → pascal name → kPrimitiveTable` is sufficient.
- **Whether function records (tag `0x28`) carry type-of-return / param
  list info.** Out of scope for B-lite; revisit at M2 phase A.
- **Line-table encoding.** Deliberately not pursuing — `.map` is
  authoritative (decision D-016).

## Open questions

1. Is the file always little-endian? (Presumed yes; consistent so far.)
2. Are address fields stored as 64-bit anywhere, or only as 32-bit
   offsets relative to a segment/ImageBase implied by header field
   0x18?
3. How are sets / variants / dynamic arrays / interfaces / managed
   strings encoded? (Out of scope until M2 phase A.)
4. Is the source-file table per-module or global?

## Provisional structural model

```
RSM file
  Header (0x00..0x1F)
    magic "CSH7"
    metadata_start = 0x420
    unit_count
    version_minor
    timestamp
    flags
    legacy_imagebase (= 0x00400000)
    padding
  EXE path string (null-terminated, starting at 0x20)
  Zero padding up to 0x420
  Small sub-header (0x420..0x425, 6 bytes — possibly metadata-block magic)
  Metadata stream (0x426..end)
    Search paths block
    DCC_UnitAliases
    EXE path (repeated)
    RTL unit table (~3.4 MB)
    User-code records
      Variable records (tag 0x20)
      Function records (tag 0x28)
      Function-aux records (tag 0x27 — SEH; skip)
      Type-definition records (tag 0x03 enum; others TBD)
    Trailing source-dir path
```
