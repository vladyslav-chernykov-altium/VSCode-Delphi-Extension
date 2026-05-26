# CLAUDE.md — orientation for future sessions

If you're a new Claude (or human) picking this project up, **read this file
first**, then `todo.txt`, then `rsm-format.txt`. Everything else follows.

---

## Project at a glance

**rsm2pdb** converts Delphi's `.map` + `.rsm` debug-info into DWARF and
injects the DWARF into the matching PE, so VSCode + gdb can debug Delphi
Win64 binaries. Comes with a turnkey VSCode extension that drives the
whole pipeline.

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
| Step 10.4 — fix override-method `Self` offset (currently shows wrong value for overrides; non-Self params + locals correct) | ⏳ deferred |
| Step 10.5 — precise primitive typing (Cardinal vs Integer, Double vs Int64, ...) | ⏳ deferred |
| Step 11 — full records / enums / classes | ⏳ deferred |
| Synthesise line entries at begin/end (begin/end-line source nav) | ⏳ deferred |
| M3 — PDB backend | ⏳ deferred |

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
| `src/model/` | Debugger-agnostic intermediate IR. |
| `src/dwarf/` | DWARF v5 emitter (hand-rolled, LLVM for constants only). Includes subprogram-with-kids, base_type, array_type, formal_parameter. |
| `src/pe/` | PE section injector. |
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

18. **Override / virtual methods encode `Self` with a non-literal
    stack-offset byte.** For override methods the RSM stores Self
    with `stack_offset = +4`, which our `(off/2)+16` formula
    decodes to rbp+18 — misaligned, clearly not Self's real
    location. For non-override methods Self has +32 -> rbp+48,
    which is correct (Delphi spills args starting at rbp+32, not
    the MS-x64 rbp+16). The `+4` value is likely a sentinel
    meaning "Self in RCX, not spilled". Non-Self params and
    locals are correct, so this is purely a Self-display issue
    on overrides. Tracked as step 10.4 in `todo.txt`.

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

1. **Step 10.4 — override-method `Self` offset fix** (~few hours to
   a day). Smallest visible cosmetic bug after M2A v2: for override
   methods (`PCBCommands_PCB.TPCBCommands.FileSave` and similar),
   `Self` is displayed at `rbp+18` (misaligned) because the RSM
   stores `+4` as a sentinel meaning "in RCX, not spilled" instead
   of a literal offset. Heuristic fix: special-case suspiciously
   small Self offsets to emit `DW_OP_reg2` (rcx) or the standard
   `rbp+8` shadow location. Full fix: decode the variable-length
   proc-trailer to find the virtual/override discriminator.

2. **Step 11 — full records / enums / classes** (~2-3 days). Most
   visible user UX win. Replaces `byte[N]` fallback for TPoint /
   TColor / classes with proper field display. RSM has the data
   (we saw the patterns); just needs decoding work.

3. **Step 10.5 — precise primitive typing** (~1-2 days). Lifts the
   D-017 same-size-merge limit (Cardinal/Single/Double distinguishable
   from Integer/Int64). Smaller win but unblocks step 11's type-ID
   reasoning.

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

*Last updated: after the M2A v2 work (real-world proc-record format
rules: scan-forward trailer + permissive subtags + graceful break on
unknown sub-record tags). gdb now sees args+locals for ~125k methods
on Altium AdvPCB; FileSave (override) shows 3 params correctly with
the caveat that override-`Self` itself is at a sentinel offset.*
