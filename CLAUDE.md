# CLAUDE.md — orientation for future sessions

If you're a new Claude (or human) picking this project up, **read this file
first**, then `todo.txt`, then `rsm-format.txt`. Everything else follows.

---

## Project at a glance

**rsm2pdb** converts Delphi's `.map` + `.rsm` debug-info into either
**PDB** (CodeView, for VS native debugger / cppvsdbg) or **DWARF v5**
(for mingw gdb / cppdbg) and injects the result into the matching PE.
PDB is the default; DWARF is opt-in via `rsm2pdb.backend = dwarf`.
Comes with a turnkey VSCode extension that drives the whole pipeline.

Status snapshot (auto-rotting; verify against the latest commit):

| Milestone | State |
|---|---|
| M1 — source-level line debugging | ✅ done, user-verified |
| M2 phase C — opaque global variables | ✅ done |
| M2 phase B-lite — typed globals (Integer + byte[N] fallback) | ✅ done |
| M2 phase A — function parameters + locals | ✅ done |
| Real-project scale (Altium AdvPCB: 120 MB DLL / 543 MB RSM) | ✅ verified, ~39s pipeline |
| **M2A v2 — real-world proc-record format** (24.6× more procs decoded; gdb sees args+locals for ~125k methods) | ✅ done 2026-05-26 |
| VSCode extension (build / debug / source nav, keybindings) | ✅ working end-to-end |
| Step 10.4 — fix override-method `Self` offset | ✅ done (subsumed by Delphi-x64 frame RE: Self always at rbp+sub_rsp+16 via name + marker check) |
| Step 10.5 — precise primitive typing (Cardinal vs Integer, Double vs Int64, Boolean, Char, ...) | ✅ done 2026-05-27 (RE'd per-unit type table → marker→Pascal name → CodeView SimpleTypeKind) |
| Step 11a — string types (`string`, `AnsiString`, `WideString`, `UTF8String`, `PChar`, etc.) | ✅ done 2026-05-27 (CodeView SimpleTypeMode::NearPointer64 + pointer-to-char base; cdb auto-displays `"..."`) |
| Step 11b — records / enums / classes (full TPI struct synthesis) | ⏳ deferred |
| Synthesise line entries at begin/end (begin/end-line source nav) | ⏳ deferred |
| **M3 — PDB backend (LLVM-backed)** — line BPs + step-into in .pas, S_GDATA32 globals + S_REGREL32 locals/params visible in cppvsdbg | ✅ done, user-verified |
| M3 follow-up — real Pascal types in PDB TPI | ✅ done 2026-05-27 (per-unit type-table RE + signed 2-byte form fix + multi-push prologue parser; all 21 ProbeLocals primitives + strings resolve in cdb) |
| Step 12 — nested-function / lambda `static link` + closure capture visibility | ⏳ deferred (placeholder `__frame_outer__` works; auto-deref to outer's fields needs TPI struct synth) |

Most current commit on `main` should explain the latest delta in its
body. `git log --oneline` for the recent history.

---

## Where things live

| Path | Purpose |
|---|---|
| `todo.txt` | **Append-only progress log + roadmap.** Sections: DONE (each milestone with what changed + why), TODO (remaining), KEY DECISIONS LOG (D-001..), OPEN QUESTIONS (Q-001..). |
| `rsm-format.txt` | **Reverse-engineering lab notebook for the RSM format.** Append-only, plain text. Tags entries `[FACT]` / `[GUESS]` / `[DISPROVEN]` / `[QUESTION]`. Hex dumps + hypotheses. |
| `docs/01-architecture.md` | Polished design view of the current layered architecture. |
| `docs/02-rsm-format-notes.md` | Polished view of what we believe the RSM format IS. The notebook is the messy ancestor of this doc. |
| `docs/03-dwarf-emission-notes.md` | DIE shapes, type map, PE-injection mechanics, CU range conventions. |
| `docs/04-validation-plan.md` | What each milestone validates and how. |
| `docs/05-vscode-extension.md` | VSCode extension architecture (module layout, parser, build runner, debug launch composition). |
| `README.md` | User-facing capabilities + "how to use on your own Delphi project". |
| `src/map/` | Delphi .map text parser + populate() adapter. Has pre-bucketed publics for scale. |
| `src/rsm/` | Delphi .rsm binary parser. Header + primitive table + variable records + procedure records (params/locals). |
| `src/model/` | Debugger-agnostic intermediate IR. `LocalVar::stack_offset` is the **rbp-relative real byte offset**, populated by `compose::resolveFunction` -- not raw RSM. |
| **`src/compose/`** | **Single source-of-truth Delphi-x64 frame interpreter.** Takes a `rsm::ProcedureRecord` + the function's PE bytes and returns `ResolvedFunction{sub_rsp, extra_pushes, vector<ResolvedVar>}`. Both PDB and DWARF emitters consume this. The frame formula (`real = sub_rsp + RSM/2` for locals, `+ 8*extra_pushes` shift for params/Self, Self at rcx-shadow, static-link synthesis) lives **only** here -- backends must not re-derive it. |
| `src/dwarf/` | DWARF v5 emitter (hand-rolled, LLVM for constants only). Consumes `model::Symbol::params/locals` whose `stack_offset` is already rbp-relative; emits `DW_AT_frame_base = DW_OP_breg6(0)` + `DW_OP_fbreg(stack_offset)`. |
| `src/pdb/` | PDB writer (LLVM-backed via `PDBFileBuilder` + `SymbolSerializer`). Emits Info/DBI/TPI/IPI/GSI streams, S_PUB32 publics, S_GDATA32 globals, per-module S_GPROC32+S_FRAMEPROC+S_REGREL32+S_END, C13 line subsections, SectionHdr DbgStream, SectionContribs. `ModuleLocal::offset` is rbp-relative real (post-compose). |
| `src/pe/` | PE section injector + tiny disassembler helpers. `pe_injector.cpp` writes DWARF sections; `pe_pdb_injector.cpp` adds the RSDS Debug Directory entry pointing at the .pdb. `prologue.h` decodes Delphi-x64 prologues (`{sub_rsp, extra_pushes}`); `size_sniffer.h` walks the body for `mov [rbp+disp], reg` widths. Both feed `compose`. |
| **`src/cli/`** | **One subcommand per file.** `cli.h` declares all entry points; `cli_cmd_<name>.cpp` implements one. `main.cpp` is dispatch only (~40 lines). Shared helpers in `util.h` (`extLower`) and `source_path.h` (`resolveSourcePath`). Adding a subcommand = new `cli_cmd_*.cpp` + a line in `cli.h` + a dispatch case in `main.cpp` -- never extend `main.cpp` body. |
| `examples/01_hello/` | Single-file Delphi sample. |
| `examples/02_two_units/` | Multi-unit (Geometry + App.Colors + dpr). Primary local-debug fixture. |
| `examples/03_primitives/` | 13 user globals across 12 distinct primitive types. Used to RE the type-marker encoding. |
| `test/fixtures/` | Committed .exe/.rsm/.map sample inputs (all three example projects). |
| `spike/` | History of the de-risking spikes. Not compiled. See `spike/README.md`. |
| `scripts/install-deps.ps1` | One-time installer for MSYS2 + LLVM. |
| `scripts/delphi-debug.ps1` | Convenience PowerShell wrapper; superseded by vscode-ext for interactive use. |
| `vscode-ext/` | TypeScript extension — **production-grade**. Build/Make/Rebuild/Debug/Cancel commands with keybindings. See `docs/05-vscode-extension.md`. |
| `.vscode/` | tasks.json + launch.json + settings.json for the rsm2pdb workspace itself. |

---

## Workflow conventions (agreed during sessions)

1. **Step-by-step.** Don't batch multiple features. Confirm scope before
   each milestone; agree what's "in" and what's deferred.
2. **Ask before destructive operations.** `rmdir build`, `git reset
   --hard`, force-push, deleting many files at once — list what
   you'd do and wait for confirmation. The user once had to manually
   recover from a wrong `rm`.
3. **If the direct approach fails, agree on the workaround first.**
   Don't unilaterally pivot from "use LLVM DWARFContext for testing"
   to "byte-level tests" — describe the failure, propose options,
   wait.
4. **Two-doc system for the RSM format:**
   - `rsm-format.txt` (lab notebook, append-only, raw hex + hypotheses)
   - `docs/02-rsm-format-notes.md` (polished, current-best-understanding)
5. **Two-doc system for the project as a whole:**
   - `todo.txt` (running log + roadmap)
   - `docs/` (polished design docs)
6. **Two-doc system for the VSCode extension:**
   - `vscode-ext/README.md` (user-facing: commands, keybindings, settings)
   - `docs/05-vscode-extension.md` (architecture-level design notes)
7. **Tests-first style is welcome but pragmatic.** doctest unit
   tests for parsers; byte-level structural tests for the DWARF
   emitter (LLVM's DWARFContext as a reader proved finicky for in-
   memory sections — gave up after ~20 min, switched to byte-level
   asserts).
8. **Verbosity preference:** show what's happening at key phase
   boundaries; don't dump every compile command. `rsm2pdb dwarf`
   prints per-phase timings (`[ N.NNs] phase: ...`). Long-running
   commands belong in `run_in_background` with notification on
   completion.
9. **The user auto-approves obvious read-only operations**
   (`ls`, `grep`, `od`, hex dumps, gdb in batch mode). Don't ask
   permission for these. Do ask before writing files outside the
   workspace, deleting things, or running long builds.
10. **Bash on Windows uses Unix syntax** but the workspace is at
    `c:/Dev/Src/rsm2pdb/` with forward slashes. Bash is fine for
    simple stuff; for CMake builds we go through `cmd.exe /C` with
    `vcvars64.bat` because that's how MSVC's environment works.
11. **`scripts/check-vs.cmd` was deleted** — don't recreate it.

---

## Architecture rules (binding for all future work)

These were locked in by the three-stage refactor on 2026-05-28
(commits `a8768f1`, `9cd88f8`, `1d6338b`). Future contributions
must follow them; if a future task seems to require breaking one,
update this section first and explain why in the commit message.

1. **The Delphi-x64 frame interpreter is `src/compose/` and only
   `src/compose/`.** `compose::resolveFunction` takes a
   `rsm::ProcedureRecord` + the function's PE bytes and returns a
   `ResolvedFunction` with rbp-relative offsets, byte sizes, and
   `PrimitiveKind`s for every param / local / Self / static-link.
   PDB (`cli_cmd_pdb`) and DWARF (`cli_cmd_dwarf`) both consume it.

   - Backends must not re-derive `sub_rsp` / `extra_pushes` /
     `param_shift` / `Self`-rcx-shadow / `__frame_outer__` synthesis
     on their own. If a new fact needs encoding (e.g. a new Self
     marker variant, a new prologue shape), add it to `compose`,
     not the backend.
   - `pe::parsePrologue` and `pe::sniffStackVarSizes` are
     compose's helpers; they're public so tests can use them but
     are not consumed directly by any backend.

2. **`model::LocalVar::stack_offset` is the rbp-relative real
   byte offset**, post-`compose::resolveFunction`. Not the raw RSM
   value. The legacy `(rsm/2) + 16` formula is gone from both
   backends -- don't reintroduce it.

3. **`src/cli/` holds one subcommand per file.** `cli.h` is the
   declaration surface; each `cli_cmd_<name>.cpp` implements
   exactly one `cli::cmd<Name>(...)` entry point. `main.cpp` is
   pure dispatch (~40 lines) -- never extend its body with new
   subcommand logic.

   - Adding a subcommand: create `src/cli/cli_cmd_<name>.cpp`, add
     its declaration to `cli.h`, add a dispatch case in
     `main.cpp`, register the .cpp in `src/CMakeLists.txt`.
   - Shared cross-subcommand helpers live in `src/cli/util.h` /
     `src/cli/source_path.h` (header-only). Things shared across
     more than just CLI go to their natural module (`src/pe/`,
     `src/compose/`, ...).

4. **Tests stay green at every commit.** The compose + cli split
   was kept regression-free by re-running the cdb + gdb checks
   after each stage. Future structural changes follow the same
   rule: build + `rsm2pdb_tests` + at least one cdb / gdb
   verification before committing.

5. **Per-emitter divergence on frame layout is a regression.** If
   PDB and DWARF show different values for the same local at the
   same line, the bug is in `compose` (or in one of the backend's
   adapters around it), never in "the other backend's formula" --
   because there is no other formula.

---

## Gotchas we hit (worth knowing, may bite again)

1. **Windows Defender / corporate EDR blocks post-link-modified PEs.**
   `objcopy --strip-debug foo.exe` followed by running `foo.exe` gets
   "Access is denied." Even local user-level Defender exclusions are
   overridden by corporate EDR on this machine. **Fix:** integrate
   DWARF injection into the build pipeline so the .exe is "born" with
   DWARF instead of being patched post-hoc. See
   `examples/*/build.cmd` and `vscode-ext/`'s buildRunner.

2. **gdb's Pascal expression parser treats `.` as field access.**
   So a watch on `two_units.S` fails with `-var-create: unable to
   create variable object`. **Fix in our adapter:** variable symbols
   get the UNQUALIFIED local name (`S`), function symbols stay
   qualified (`Geometry.Add` — important for stack traces). See
   `src/map/map_reader.cpp populate()`.

3. **LLVM's CMP0091 NEW behaviour needs to be activated before
   `project()`.** Setting `CMAKE_MSVC_RUNTIME_LIBRARY` after
   `project()` is too late; it needs `set(... CACHE STRING ""
   FORCE)` BEFORE the `project()` call to propagate to the toolchain
   probe. Verify with `grep MSVC_RUNTIME build/CMakeCache.txt` after
   `cmake --fresh`.

4. **doctest 2.4.11's `cmake_minimum_required` is too old** for the
   bundled VS2026 CMake. Workaround: `set(CMAKE_POLICY_VERSION_MINIMUM
   3.5)` in `test/CMakeLists.txt` before the FetchContent call.

5. **DWARF v5 line program state machine** initialises the `File`
   register to 1, but our file table has only entry 0. Without an
   explicit `DW_LNS_set_file 0` at program start, every emitted row
   references file 1 (out of range) and gdb silently drops them.
   See `src/dwarf/dwarf_emitter.cpp writeLineProgram()`.

6. **VSCode's cppdbg only allows breakpoints in known languages.**
   Pascal isn't in that list. Need `"debug.allowBreakpointsEverywhere":
   true` in `.vscode/settings.json`. Without it, gutter clicks on
   `.pas`/`.dpr` files silently do nothing.

7. **`build` task needs `vcvars64.bat` sourced** when invoked from
   VSCode (since VSCode doesn't run the VS Dev shell). Our
   `.vscode/tasks.json` runs `cmd /c "call vcvars64.bat && cmake
   --build ..."` to handle this. If you see `link.exe` complaining
   about missing libs, this is why.

8. **VS2026 (= "VS18") installs at `C:\Program Files\Microsoft
   Visual Studio\18\Insiders\` — NOT `(x86)`.** Legacy `vswhere.exe`
   at `${ProgramFiles(x86)}\Microsoft Visual Studio\Installer\` doesn't
   see VS18. `scripts/install-deps.ps1` has a fallback `Find-VSInstall`
   that probes the new location.

9. **`spike/inject_dwarf.c` is historical only.** The production
   injector is `src/pe/pe_injector.{h,cpp}` — a C++ port that handles
   `SizeOfHeaders` expansion. Don't add new logic to the spike.

10. **Delphi RSM format magic is "CSH7" (not "FB09"/"FB0A").** JCL's
    `JclTD32.pas` reads the legacy TD32 format which is similar in
    spirit but not directly applicable. The RSM parser was largely
    reverse-engineered from scratch. See `rsm-format.txt`.

11. **`rsm2pdb dwarf` requires Delphi to NOT embed TD32 in the EXE**
    (set `DCC_DebugInfoInExe=false`). Otherwise the EXE has a 1+ MB
    `.debug` section that uses up PE header slots, and the injector
    fails with "not enough header space". `examples/*/two_units.dproj`
    has this set correctly. AdvPCB.dproj does NOT set it explicitly
    (defaults on); the in-place injection still worked because the
    DLL had enough header headroom — fortunate, not guaranteed.

12. **CU `low_pc`/`high_pc` MUST include function symbol addresses,
    not just line-entry addresses.** Delphi's compiler-generated
    unit-init function `<Unit>.<Unit>` (the .dpr's `begin..end`
    block) often has sparse / no line entries. If we derive CU
    range only from line entries, that function falls outside the
    range -> gdb returns "no source" for any address in it ->
    VSCode can't navigate. Fixed in `src/dwarf/dwarf_emitter.cpp
    cuRange()` by absorbing function symbol addresses too. Commit
    d6d6abf.

13. **gdb's MI `-environment-directory` accepts a quoted path arg
    as a single C-string and preserves spaces.** The console-syntax
    `directory <path>` does NOT — it splits on whitespace, so
    `C:/Source code` becomes two paths (`C:/Source` and `code`).
    Always use the MI form when emitting source-search dirs from
    the extension. See `vscode-ext/src/extension.ts`.

14. **VSCode cppdbg `sourceFileMap` does prefix substitution, not
    file-equality.** Bare-filename keys like `"AdvPCB.dpr"` only
    work as exact-match prefixes for paths that gdb reports
    unqualified — which depends on whether gdb resolved the path
    or not. Prefer `-environment-directory` so gdb returns
    fullname; sourceFileMap is a useful belt-and-braces but not
    a primary mechanism.

15. **VSCode `DiagnosticSeverity.Hint` does NOT show as a PROBLEMS
    row** — only as inline editor decorations. Delphi compiler
    "Hint" diagnostics must be mapped to `Information` instead to
    surface in the panel. See `vscode-ext/src/buildRunner.ts`.

16. **The PE injector renames pre-existing `.debug_*` sections to
    `.old_dw` before appending fresh ones.** Without this, a DLL
    that's been injected once before ends up with TWO `.debug_info`
    sections; gdb reads the FIRST (stale) one and reports "no
    locals or arguments" even when the new DWARF carries them.
    This bit us on AdvPCB the first time round (the input DLL was
    already an output of a prior `rsm2pdb dwarf` run). See
    `src/pe/pe_injector.cpp` (search for `.old_dw`). Side note:
    each re-injection cycle leaves the old `.old_dw` in place;
    accumulation is harmless but cosmetically ugly.

17. **Real Delphi proc records are MUCH more varied than the small
    fixtures showed.** The original M2-phase-A parser hard-coded
    a fixed 16-byte head (assumed 5-byte trailer between VA and
    sub-records), a single proc subtag value (`0xA0`), and two
    sub-record subtag values (`0x66`/`0x62`). On real binaries
    that's 2% coverage. Reality:
      - Proc subtag ∈ {0xA0, 0xE0, 0x80}.
      - Trailer between VA and first sub-record is variable
        (3..30 bytes; the "5 bytes" was just one common case).
      - Sub-record subtag is variable (~10 values); the strong
        invariant is the two zero bytes at +1,+2 after it.
      - Real procs carry additional sub-records with tags `0x23`
        (return-value descriptor) and `0x25` (enum entries) after
        the param/local list.
    Application rules in `src/rsm/rsm_reader.cpp scanProcedures()`:
    permissive subtag acceptance, scan-forward to find first valid
    sub-record / true end marker, graceful break on unknown sub-
    record tags. See `docs/02-rsm-format-notes.md` (Procedure
    record section) and `rsm-format.txt` (2026-05-26 entry).

18. **`Self` in class methods carries a sentinel `stack_offset`
    (not a real frame offset).** RSM tags Self with a non-primitive
    marker:
      - 0x29 for base-class virtual / regular methods
      - 0x31 for override methods
      - 0xD5 for anonymous-method closure Self (ActRec.$0$Body)
    paired with sentinel offsets `+4109` (newer Delphi) or `+4`
    (older). Don't decode these via the general `sub_rsp + RSM/2`
    formula -- it lands well outside any real frame slot. Instead,
    Self always lives at `rbp + sub_rsp + 16 + 8*extra_pushes` (=
    the rcx-shadow slot, where Delphi spills the implicit `this`-
    pointer; the extra_pushes term shifts it up if the prologue
    pushed callee-saved regs between `push rbp` and `sub rsp`).
    main.cpp routes Self through this special case by checking
    `name == "Self" || marker == 0x29 || marker == 0xD5` -- the
    name fallback also catches 0x31 (override) so we don't need to
    enumerate every marker variant. Verified on examples/04_locals
    against TBase + TDerived virtuals/overrides.

19. **PDB writer (LLVM-backed) gotchas — `cppvsdbg` is strict.**
    Building a PDB that VS native debug actually accepts took
    multiple rounds of fixing. Surface area in
    `src/pdb/pdb_writer.cpp`:
      a. `PDBFileBuilder::initialize()` does NOT allocate the 5
         reserved stream slots. Call
         `getMsfBuilder().addStream(0)` `kSpecialStreamCount` times
         immediately after `initialize()`, or `commit()` fails with
         "stream too short".
      b. DBI requires `DbgStream[SectionHdr]` (the COFF section
         headers verbatim). Without it, vsdbg loads symbols but
         throws "Unexpected symbol reader error" when resolving a
         breakpoint.
      c. File checksums must be a real hash (MD5 minimum); kind
         `None` silently breaks BP resolution in cppvsdbg.
      d. `S_GDATA32` / `S_REGREL32` with `TypeIndex::None()` are
         invisible in Watch/Locals. Until M3-followup ships real
         types, default everything to `Int32` (raw 4-byte view).
      e. `ProcSym.End` must be patched to point at the matching
         `S_END`'s module-stream offset. Use `getNextSymbolOffset()`
         before+after to compute it, then patch the proc record's
         bytes in place (offset 8 = 4-byte CVHeader + 4-byte Parent).
         End=0 means the scope is "open" and Locals stays empty.
      f. SectionContribs must be sorted by `(ISect, Off)` before
         submission — vsdbg binary-searches them. Adding in module
         iteration order leaves SCs interleaved and BP resolution
         fails for whichever module lands at "the wrong" position.
      g. Per-module `mod_index` increment must be UNCONDITIONAL.
         A `continue` that skips it (e.g. for modules without C13
         lines) silently mis-attributes every later module's SCs.
      h. `S_FRAMEPROC` is required for cppvsdbg to resolve
         RegRel-based locals. Encoded frame ptr reg bits 14-15
         (local) and 16-17 (param) must both be 2 (`FramePtr`,
         which decodes to RBP on x64).
      i. cppvsdbg's expression evaluator treats `.` as field
         access. Qualified Pascal names like `two_units.S` parse
         as "field S of two_units" and report "two_units is
         undefined" in Watch. Strip the module prefix in
         `S_GDATA32` names; keep the fully-qualified name in
         `S_PUB32` so stack traces stay informative.
      j. RSDS file checksum / Image file checksum mismatch is
         tolerated by vsdbg as a "source-file changed" warning
         but BPs still bind, so MD5 of the resolved source path
         is good enough.

20. **PDB Section header file offsets don't match the patched
    PE.** The PDB stores the PE's section table BEFORE our RSDS
    section gets appended. After our `injectPdbReference`
    extends the PE (and possibly bumps file offsets via header
    expansion), the file offsets in the PDB's SectionHdr stream
    are stale. They don't matter for runtime resolution (which
    is RVA-based) but `llvm-readobj --coff-debug-directory`
    against the patched PE will sometimes complain about
    "uneven size" if a prior debug directory entry survived.
    Cosmetic; debug works.

21. **PDB source path stored in `C13 FileChecksums`** is what
    cppvsdbg uses to match breakpoint file paths. Delphi's
    `.map` carries only basenames (`Geometry.pas`); we resolve
    against (`--src-search` dirs from the extension) +
    (mapDir/.., mapDir, mapDir/../..) and call
    `std::filesystem::canonical()` so the PDB ends up with
    absolute paths. Without absolute paths, cppvsdbg fails to
    bind BPs even with `sourceFileMap` set — its matcher is
    less flexible than gdb's.

22. **Delphi-x64 prologue variants — `parsePrologue` handles
    both.** The single-block shape `push rbp; sub rsp, imm;
    mov rbp, rsp` is the common case (every proc in
    `examples/04_locals`), but procs with managed locals or many
    spills push callee-saved registers between `push rbp` and
    `sub rsp`:

        push rbp
        push rdi          \  zero..many of these (typical pairs are
        push rsi           > rdi+rsi, or r12..r15 via REX.B + 0x54..)
        sub  rsp, 0x1C0   /
        mov  rbp, rsp

    `parsePrologue` returns `{sub_rsp, extra_pushes}`. Locals don't
    shift (they live at `rbp+0 .. rbp+sub_rsp` regardless of
    pushes), but params + Self shift up by 8 bytes per extra push:
    `real_param = sub_rsp + 8*extra_pushes + RSM/2`. The pushes
    occupy the slots immediately above the local-area, so the
    rcx-shadow slot moves from `rbp + sub_rsp + 16` to `rbp +
    sub_rsp + 16 + 8*extra_pushes`. Verified on
    `examples/05_types ProbeLocals` (2 extra pushes -> all 21
    primitive/string locals resolve correctly in cdb).

23. **Per-unit type table layout (RSM unit metadata block).** Each
    Pascal unit's metadata block in RSM starts with the unit anchor
    `02 <namelen> <unit-name>` and is followed within ~200 bytes by
    a source-file ref `02 70 <namelen> <file>.{pas,dpr,inc,tmp}`.
    The primary type table is a sequence of `0x66` records (each
    `0x66 <namelen> <pascal_name> <4-byte hash>`) in source-
    declaration order, **interleaved with `0x67` records** that
    reference imported functions / methods of the same `<tag>
    <namelen> <name> <hash>` shape -- the scanner walks past them
    without recording. Variable's `type_marker` (always even,
    `0x02 / 0x04 / 0x06 / ...`) indexes the table: marker = `2 *
    (index + 1)`. Looking name up in the hard-coded
    `kPrimitiveTable` (in `rsm_reader.cpp`) gives `(PrimitiveKind,
    byte_size)` -- the pair the pdb_writer needs for typed
    S_REGREL32 / S_GDATA32 emission. False positives are avoided
    by tying every collected table to its unit anchor (per-anchor
    scan with a hard upper bound) rather than free-floating
    "longest run of 0x66" detection, which used to mis-attribute
    variables across units (every marker 0x02 in `examples/
    04_locals` resolved as Boolean before this fix because the
    "locals" unit's anchor-less table was discarded).

24. **Stack-offset 2-byte encoding is SIGNED i16.** Sub-record
    payloads carry the stack offset in either a 1-byte form (LSB=0,
    decoded as `(int8) b0`) or a 2-byte form (LSB=1, two bytes
    `b0, b1` forming a little-endian word). The 2-byte form must be
    interpreted as a **signed** 16-bit value, then `stored_off =
    ((int16) v - 1) / 2` to keep the downstream `sub_rsp + stored
    / 2` formula working. Empirically:

        v=0xFE61 (i16=-415)  -> stored=-208  -> real = sub_rsp + (-104)
        v=0xF9A1 (i16=-1631) -> stored=-816  -> real = sub_rsp + (-408)

    The earlier unsigned read gave `+32560 / +31952` which sent
    these locals well past the end of any real frame. This bites
    string / bool / char / pointer locals in any non-trivial proc
    (their negative-from-top-of-frame offset doesn't fit a signed
    i8). After the fix all 21 of `examples/05_types ProbeLocals`'s
    primitives + strings resolve in cdb -- including
    `lO=true / lCh='Z' / lS="unicode" / lAS="ansi" / ...`.

25. **CodeView string display uses pointer-to-char *simple* types,
    no LF_POINTER record needed.** Combining
    `SimpleTypeKind::WideCharacter / NarrowCharacter` with
    `SimpleTypeMode::NearPointer64` synthesises
    `TypeIndex = T_64PWCHAR / T_64PRCHAR`. cdb and the VS native
    debugger auto-display the pointee as a string starting at the
    address until the first NUL. Pascal RTL conveniently puts char
    data at the pointer's exact address (header bytes live just
    below), so `UnicodeString` / `AnsiString` / `WideString` /
    `UTF8String` / `PChar` / `PAnsiChar` / `PWideChar` all work
    without any extra TPI work. `ShortString` is **not** a managed
    pointer -- it's an inline length-prefixed byte array -- and
    falls back to the size-based `void*` / `byte[N]` chain.
    `WordBool` / `LongBool` use `SimpleTypeKind::Boolean16 /
    Boolean32` which cdb refuses to render (reports "Type
    information missing"); the bit values are still readable via
    `dt` / `dd`. Cosmetic only.

26. **Nested-function subtag is 0x41, not the usual `{A0,E0,80,
    20}`** -- with 3 mystery bytes between subtag and VA (`02 10
    00 <VA u32>`) rather than the usual `00 00 <hash u32> <VA
    u32>`. Parser must accept it without the `00 00`-after-subtag
    invariant. The static link (parent's rbp, passed in rcx) is
    NOT carried as a sub-record; the parser sets
    `ProcedureRecord.has_static_link = true` on subtag 0x41 so
    main.cpp can synthesise a `__frame_outer__` placeholder local
    at `rbp + sub_rsp + 16`. Lambda body procs
    (`<Outer>$ActRec.$0$Body`) use the regular subtag but carry
    Self with marker 0xD5 -- their captured fields live in
    `[Self+0x10..]` and need TPI struct synth for auto-deref
    (Step 12).

---

## How we maintain `todo.txt` and `rsm-format.txt`

- **`todo.txt`** is the running project log. Format:
  - Top: goals, scope, what's in/out of v1.
  - `DONE` section: each finished milestone with what changed + why.
    New milestones get prepended (so most-recent-first).
  - `TODO` section: numbered steps with effort estimates, verification
    notes, what they block.
  - `KEY DECISIONS LOG`: D-001, D-002, ... numbered decisions with one-
    paragraph rationale. NEVER renumber; if a decision is overridden,
    add a new one explaining the override.
  - `OPEN QUESTIONS`: Q-001..; some get answered and stay (with the
    answer) as historical record.

- **`rsm-format.txt`** is append-only with date headers. When adding:
  - Use `[FACT]`, `[GUESS]`, `[DISPROVEN]`, `[QUESTION]` tags.
  - Include hex dumps at relevant offsets when you have them.
  - Mark hypotheses you've tested even if they failed — saves future
    re-investigation.

- **`docs/02-rsm-format-notes.md`** is the polished view. Update it
  when our understanding consolidates. The lab notebook is the messy
  source of truth; the polished doc is the current best summary.

---

## Resume here

**Last commit:** see `git log --oneline | head -3`.

**Three candidate next steps:**

1. **Step 11b — records / classes / enums (full TPI struct synth)**
   (~2-3 days). Biggest remaining UX win. RSM marks non-primitive
   variables with `byref t=0xNNNN` (inline type id into the per-unit
   type table); we currently fall back to `byte[N]` for these. With
   TPI `ClassRecord` + `FieldList` synthesis, TPoint / classes /
   user records would show field-by-field in Watch. Same machinery
   also unblocks closure capture display (Step 12, lambdas' ActRec).

2. **Step 12 — closure visibility** (~half a day, depends on 11b).
   Nested-function static-link and lambda-body Self are already
   emitted as 8-byte pointers (`__frame_outer__` and `Self`
   respectively); user can manually deref via
   `dd poi(@rbp+0x20)+0x40` to reach outer's vars. Auto-deref
   needs synthesising a TPI struct for the outer frame (or ActRec)
   and re-typing the pointer; reuses the same machinery as 11b.

3. **WordBool / LongBool display in cdb.** Cosmetic regression --
   cdb doesn't render `SimpleTypeKind::Boolean16` / `Boolean32`
   ("Type information missing"). The bit values are readable via
   `dd`/`dt`. Could fall back to `UInt16` / `UInt32` and lose the
   bool-as-`true/false` UX, or emit a tiny custom TPI enum. Low
   priority; only affects 4-byte and 2-byte Pascal booleans (rare).

If working on RSM RE, recall the offsets and conventions are
documented in `docs/02-rsm-format-notes.md` (procedure-record
section is the freshest) and `rsm-format.txt` (2026-05-26 entry
covers the proc-format empirical findings).

**Diagnostic subcommands available** (added during M2A v2 work,
keep them — they're invaluable for future RSM RE):
- `rsm2pdb diff-procs <map> <rsm>` — VA cross-check; reports
  missing-in-rsm count with raw-name-in-bytes probe.
- `rsm2pdb probe-procs <map> <rsm>` — stratified sample of failed
  proc-records, hex + decoded.
- `rsm2pdb analyze-procs <map> <rsm>` — single-pass aggregate
  histograms (proc subtag, trailer length, sub-record subtag) over
  the whole .rsm file.

**Don't:**
- Reverse the line tables — D-016 established `.map` is authoritative.
- Use DWARFContext for testing — see workflow rule #7.
- Broaden allowlisting beyond read-only commands in
  `.claude/settings.json`.

---

*Last updated: 2026-05-28, after the three-stage architectural
refactor (a8768f1 + 9cd88f8 + 1d6338b). All Delphi-x64 frame logic
now lives in `src/compose/`; PDB and DWARF backends both consume
`ResolvedFunction` from it. main.cpp is dispatch-only (~40 lines);
each subcommand has its own `src/cli/cli_cmd_*.cpp`. gdb on the
DWARF output now matches cdb on the PDB output to the byte (verified
on examples/04_locals GlobalProc2). 59 unit tests green. Binding
architecture rules captured above; future work must follow them.
Remaining UX gaps are Step 11b (records / classes / enums via TPI
struct synth) and Step 12 (closure auto-deref reusing the same
machinery).*
