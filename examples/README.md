# Delphi examples

Small Delphi 64-bit programs that exercise progressively more of the
language so we can validate `rsm2pdb` against known inputs.

| Example       | Exercises                                  |
|---------------|--------------------------------------------|
| 01_hello      | functions, locals, records, enums, globals |
| 02_arrays     | static + dynamic arrays (planned)          |
| 03_strings    | ShortString, AnsiString, UnicodeString     |
| 04_classes    | class with methods + VMT                   |

## Building

Each subdirectory has a `build.cmd` that invokes `dcc64.exe` directly so we
don't need a `.dproj`. Pass the dcc64.exe path as the first argument, or put
it on PATH first.

Compiler switches we standardize on:
- `-B`  build all units (no stale `.dcu` reuse)
- `-V`  emit TD32-family debug info
- `-VR` write symbols to `.rsm` (remote) instead of embedding in EXE

The resulting `.exe` + `.rsm` pair belongs under `test/fixtures/` once it's
validated as a good sample.
