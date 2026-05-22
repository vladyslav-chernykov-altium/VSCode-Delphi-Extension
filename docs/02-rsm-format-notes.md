# RSM format notes

Embarcadero's `.rsm` (Remote Symbol) file is the debug-info container
that ships alongside Win64 EXEs when Delphi is built with debug info on
and Win64 platform. Generated automatically by the Delphi linker for
Win64 targets.

**Update after first fixture inspection (May 2026):** the format is
NOT the legacy Borland TDS/TD32 layout. The magic is ASCII "CSH7", not
"FB09"/"FB0A". The Embarcadero RSM format appears to be a redesign,
not just a repackaging of TD32. JCL's `JclTD32.pas` is therefore an
intellectual-history reference, not a translation source.

## Primary public reference

- **JCL (Jedi Code Library)** - `JclTD32.pas`, `JclDebug.pas`, `JclPeImage.pas`
  - Repo: github.com/project-jedi/jcl
  - Read-only TD32 parser; covers headers, subsection directory, names
    pool, modules, source files, line numbers, symbols, types.
  - We translate the *knowledge*, not the code (license + language barrier).

## Secondary references

- Borland TASM / TD32 specification leaks circulated in the 1990s; usable
  as cross-checks against JCL but not authoritative.
- Microsoft CodeView documents (CV4 / CV5) - TD32 borrows record kinds
  from CodeView but extends them. Useful to disambiguate type-record fields.

## First-fixture findings (hello.rsm, 3.5 MB, Delphi 10.2 Tokyo Win64)

Hex dump of bytes 0-63:

```
offset  hex                                              ascii
0000    43 53 48 37 20 04 00 00 0e 00 00 00 01 00 00 00  CSH7 ...........
0010    17 52 b6 5c 4d 23 00 20 00 00 40 00 00 00 00 00  .R.\M#. ..@.....
0020    2e 5c 57 69 6e 36 34 5c 44 65 62 75 67 5c 68 65  .\Win64\Debug\he
0030    6c 6c 6f 2e 65 78 65 00 00 00 00 00 00 00 00 00  llo.exe.........
```

Initial guesses (NOT YET VERIFIED - validate by parsing several fixtures):

| Offset | Bytes        | Interpretation (guess)                       |
|-------:|--------------|----------------------------------------------|
| 0x00   | `43 53 48 37`| **Magic "CSH7"** (ASCII)                     |
| 0x04   | `20 04 00 00`| uint32 = 0x420 = 1056. Header / directory offset? |
| 0x08   | `0e 00 00 00`| uint32 = 14. Subsection count or major version? |
| 0x0C   | `01 00 00 00`| uint32 = 1. Minor version / flags?           |
| 0x10   | `17 52 b6 5c`| uint32 = 0x5CB65217 = Apr 17, 2019 (epoch). Build timestamp? |
| 0x14   | `4d 23 00 20`| uint32 = 0x2000234D. Unclear. Maybe high half of something. |
| 0x18   | `00 00 40 00`| uint32 = 0x00400000. Default 32-bit ImageBase value. Curious for a Win64 file. |
| 0x1C   | `00 00 00 00`| padding |
| 0x20   | `.\Win64\Debug\hello.exe\0` | Null-terminated path to target EXE this RSM describes. |

The "CSH" magic might stand for something like "Code Symbol Hierarchy"
or be a build / repository codename. It is in modern (10.x+) RSM files
and may not be in older ones.

### Cross-references we have for this fixture

- `test/fixtures/hello.exe`  - the binary the RSM describes
- `test/fixtures/hello.map`  - human-readable map file (best ground truth)
- `test/fixtures/hello.rsm`  - the file we're parsing

We will validate the parser by checking that addresses / symbol names
from RSM match what's in `hello.map`.

## Structural model (provisional, TO VERIFY)

```
RSM file
  Header
    magic            "CSH7" (4 ASCII bytes)
    ??? (8 fields, total 60 bytes including the EXE-path string)
    EXE path         null-terminated, up to some bounded length
  Directory ???      followed by subsections
  Subsections
    Modules          (per-CU info)
    Source files
    Public symbols
    Local symbols
    Line numbers
    Types
    Strings
    Segments / address ranges
```

This is still guesswork. Concrete steps to validate:
1. Cross-reference offsets against the `.map` file to identify symbol
   tables in the RSM.
2. Compare RSM and EXE TD32 sections - the EXE has debug info embedded
   too (we set DebugInfoInExe=true), and the same data presumably
   appears in both. If the EXE TD32 section is more documented (CV4
   leaks from the 90s), we can use it as a Rosetta stone.

## Open questions for first sample inspection

1. What's the magic / version in modern (Delphi 11+) RSM files?
2. Is the file always little-endian? (TD32 historically yes.)
3. Are 64-bit address fields used everywhere, or are there still 32-bit
   leftover fields with a separate "high dword" stored elsewhere?
4. How are Delphi-specific types (sets, variants, dynamic arrays,
   interfaces, managed strings) encoded? Do they use CodeView extension
   kinds Embarcadero added?
5. Is the source-file table per-module or global?
6. Are line numbers paired with start-of-statement only, or do we also
   get column info?

We answer these by hex-dumping `test/fixtures/hello.rsm` once it's
available.
