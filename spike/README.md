# spike/ — de-risking history (read-only reference)

This directory is a historical record of the spikes that proved the
rsm2pdb architecture was sound. The production code lives in `src/`;
nothing under `spike/` is compiled by the main build.

## What was proven

**6a — gdb reads DWARF v5 from PE on Windows.** Built `hello.c` with
mingw-gcc and `-gdwarf-5`, verified gdb 17.2 resolves source:line and
prints stack traces with line numbers.

**6b — VSCode `cppdbg` adapter steps through that PE.** User-confirmed
in interactive VSCode session.

**6c — A PE with no DWARF can have DWARF added post-link.** Wrote
`inject_dwarf.c` (later `inject_files.c`) to demonstrate the byte-
rewriting approach: strip a PE, append `.debug_*` sections, patch
`NumberOfSections` / `SizeOfImage` / COFF string table for long
section names. gdb on the resulting PE produces identical behaviour
to the original linker-emitted PE.

After 6c, the injector logic moved into the production code at
`src/pe/pe_injector.cpp` (C++ port of `inject_files.c`).

## What's left here

| File | Why kept |
|---|---|
| `hello.c`        | 6a/6b control-test source (~25 lines) |
| `inject_dwarf.c` | Original 6c spike: takes debug sections from another PE |
| `inject_files.c` | 6c follow-on: takes debug sections from files. Direct ancestor of `src/pe/pe_injector.cpp` |

The `.exe` and `.bin` artifacts that lived here have been deleted —
they were one-off test outputs, easy to regenerate if anyone wants to
re-run the spikes.

## Re-running the spikes (if curious)

The `.c` sources still compile under mingw-gcc:

```bash
cd spike/
/c/Dev/Tools/msys64/mingw64/bin/gcc.exe -O0 -gdwarf-5 -o hello.exe hello.c
/c/Dev/Tools/msys64/mingw64/bin/gcc.exe -O2 -o inject_dwarf.exe inject_dwarf.c
/c/Dev/Tools/msys64/mingw64/bin/gcc.exe -O2 -o inject_files.exe inject_files.c
```

But you almost certainly want the production tool instead:

```cmd
rsm2pdb dwarf <input.map> <input.exe> <output.exe>
```
