# CLAUDE.md — orientation for future sessions

If you're a new Claude (or human) picking this project up, **read this file
first**, then `todo.txt`, then `rsm-format.txt`. Everything else follows.

---
## Project at a glance

**rsm2pdb** converts Delphi `.map` + `.rsm` debug-info into either
**PDB** (CodeView, for VS native debugger / cppvsdbg) or **DWARF v5**
(for mingw gdb / cppdbg) and injects the result into the matching PE.
PDB is the default; DWARF is opt-in via `rsm2pdb.backend = dwarf`.
Comes with a turnkey VSCode extension that drives the whole pipeline.

Milestone status: see the `DONE` / `TODO` sections of `todo.txt`.
Most recent commit on `main` explains the latest delta in its body
(`git log --oneline | head -3`).

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
| `src/pdb/` | PDB writer (LLVM-backed via `PDBFileBuilder` + `SymbolSerializer`). Emits Info/DBI/TPI/IPI/GSI streams, S_PUB32 publics, S_GDATA32 globals, per-module S_GPROC32+S_FRAMEPROC+S_REGREL32+S_END, C13 line subsections, SectionHdr DbgStream, SectionContribs. `ModuleLocal::offset` is rbp-relative real (post-compose). By default `cli_cmd_pdb` strips RTL publics + modules (`System.*` / `SysInit` / `Winapi.*` / `Vcl.*` / `Fmx.*` / `Soap.*` / `Datasnap.*` / `Data.*` / `Inet.*` / `IBX.*` / `REST.*` / `Bde` / `IndySystem`); pass `--include-rtl` to keep them. Cuts PDB size 50x+ on real projects -- we have no RSM proc-records for RTL units anyway, so the publics were just inflating the file and polluting Call Stack. |
| `src/pe/` | PE section injector + tiny disassembler helpers. `pe_injector.cpp` writes DWARF sections; `pe_pdb_injector.cpp` adds the RSDS Debug Directory entry pointing at the .pdb. `prologue.h` decodes Delphi-x64 prologues (`{sub_rsp, extra_pushes}`); `size_sniffer.h` walks the body for `mov [rbp+disp], reg` widths. Both feed `compose`. |
| **`src/cli/`** | **One subcommand per file.** `cli.h` declares all entry points; `cli_cmd_<name>.cpp` implements one. `main.cpp` is dispatch only (~40 lines). Shared helpers in `util.h` (`extLower`) and `source_path.h` (`resolveSourcePath`). Adding a subcommand = new `cli_cmd_*.cpp` + a line in `cli.h` + a dispatch case in `main.cpp` -- never extend `main.cpp` body. |
| `examples/01_hello/` | Single-file Delphi sample. |
| `examples/02_two_units/` | Multi-unit (Geometry + App.Colors + dpr). Primary local-debug fixture. |
| `examples/03_primitives/` | 13 user globals across 12 distinct primitive types. Used to RE the type-marker encoding. |
| `examples/04_locals/` | Procedure-locals + nested function + virtual / override methods. The Delphi-x64 frame-layout RE relied on this. |
| `examples/05_types/` | 21 primitive + string typed locals in `ProbeLocals`. RE'd Step 10.5 + 11a (precise primitive typing + string pointer-to-char). |
| `examples/06_interface/` | Interface-dispatch patterns + adjuster thunks (Item 21). Used to verify `.natstepfilter` + hybrid auto-skip. |
| `examples/07_records/` | Records / classes (with single-level inheritance) / enums / sets. Drove Step 11b phases D / E / F / F+. TBig has 40 Integer fields to exercise the 2-byte offset form (R6). |
| `examples/08_inherit_props/` | 3-level inheritance (TAnimal -> TMammal -> TDog) + Pascal `property` (field-backed / read-only / getter-setter). Surfaced the variable-length class-header bug (gotcha #26 (`docs/06-gotchas.md`)); confirmed properties are invisible to CodeView. |
| `examples/09_cross_unit/` | Multi-unit project: Shapes.pas declares TPoint/TColor/TSize, Items.pas declares TItem with those as fields, Layout.pas declares TLayout referencing both, cross_unit.dpr uses all three. Drove the cross-unit type-resolution work (gotchas #33..#36): per-unit 4-hash-per-class chaining, 0x66 imports table with 4-byte hash, multi-uses 0x63 0x64 separators, cross-unit composite field resolution via name + low-16-of-hash4. |
| `test/fixtures/` | Committed .exe/.rsm/.map sample inputs (hello, two_units, primitives, records). |
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

## Frugality directives (STRONG — review before any refactor / build / text-manip phase)

Locked in by the 2026-05-29 retro after four back-to-back refactors
(`rsm/` + `pdb/` + `dwarf/` + `cli_cmd_pdb`). The 28-budget overrun
on `cli_cmd_pdb` (vs estimated 15-18) drove a consolidation pass:
13 directives -> 11, with numeric triggers replacing soft language
("drift" -> ">5 build errors / >1.5x time / new phase surfaced").

Treat this section as a checklist, not optional reading -- glance
it before entering any refactor / multi-step build / bulk text-
manipulation phase. Grouped by phase of work.

### A. Plan (before starting)

1. **SCAN BEFORE EXTEND -- AT THE RIGHT TRIGGER.** Before adding
   a **logically new chunk / phase >= 30 lines** to an existing
   function or file, run a quick size sanity check on the target
   (`wc -l`, function bounds). NOT before every micro-edit --
   20 edits don't need 20 sanity checks. Decision rules:
   - Function: split when > 150 lines AND has > 1 distinct
     logical phase. A tight 180-line single-responsibility
     function is FINE; don't fragment-tax it into 3 x 60 lines
     that read worse.
   - File: split when > 400 lines AND has > 5 unrelated
     responsibilities. Same caveat -- cohesive medium-sized
     files beat over-shredded ones.
   The threshold is a SMELL signal triggering judgment, not an
   auto-mandate. The anti-pattern this guards against is the
   1000+ line monster grown silently from "just 50 more lines"
   accretions (rsm `open()` / pdb `writePdb()` -- both got there
   that way), not the tight 200-line function doing one thing
   well.

2. **BUDGET = PLAN, NOT PRIVACY CAP. RE-PLAN ON NUMERIC DRIFT.**
   Before any non-trivial task: surface a per-phase estimate to
   the user (scope X, phases A/B/C, est cost x/y/z). Then execute
   the full plan. DO NOT silently narrow scope mid-task to "fit"
   a budget -- the task always wins; the budget is the plan, not
   a hidden cap. **Re-plan triggers (any one fires -> STOP and
   bring revised plan to the user):**
   - First build with >5 errors after a refactor step (design
     drift -- preamble incomplete, scope underestimated).
   - Elapsed effort >1.5x the original estimate for the current
     phase.
   - A new previously-unforeseen phase surfaces.
   - An assumption from the plan turns out wrong on inspection.
   Never patch-in overruns silently. Never half-do the task to
   stay under the original number. Reference failure modes: 2026-
   05-29 dwarf/ (silent scope-cut: file split skipped to "save"
   budget; caught by user); cli_cmd_pdb (28 vs 15-18 estimated --
   first-build-fail-with-20-errors should have triggered re-plan,
   instead I plowed through fix-build cycles).

### B. Verify (during work)

3. **VERIFY AT THE SMALLEST MEANINGFUL MILESTONE.** Not every
   edit; not only at the end. A "milestone" = a self-contained
   verifiable chunk whose failure has a manageable rollback. A
   complex refactor has 3-5 internal milestones (Context design
   compiles -> 1st phase extracts cleanly -> ... -> final split).
   Don't burn cycles on full-build after every micro-edit
   (trust compiler diagnostics there); equally, don't defer
   ALL verification to the end (rollback explodes if it fails).

4. **WHEN BUILD FAILS -- BATCH FIXES.** If a build returns
   multiple errors (>=2 distinct root causes), read ALL of them
   FIRST, batch the corresponding edits into a SINGLE pass,
   THEN rebuild. Don't edit-build-edit-build cycles -- they
   compound friction. If errors >5: this is also a directive
   #2 re-plan trigger.

5. **THREE STRIKES ON A HEURISTIC.** If a discriminator gets a
   third condition (`X && Y && Z`) or a third "but on real data
   we also see..." carve-out, STOP adding clauses. Dump fresh
   hex from 3-5 real samples, find the actual format invariant,
   rewrite the discriminator. The Nth condition is technical debt
   that compounds across releases (see gotchas #17, #29, #31 in
   `docs/06-gotchas.md` for what this looks like when ignored).

### C. Execute

6. **ONE-LINER HELPERS FOR 3+ REPEATS.** The moment a command's
   boilerplate (vcvars wrapping, cd-into-build chains) appears
   3+ times, write a tiny helper script (`build/_b.sh` for build,
   `build/_t.sh` for test) and use it. Re-typing 150-char
   incantations is pure waste.

7. **ASK ABOUT TOOLING UP-FRONT.** At session start, when you
   can predict you'll run scripts / heredocs / cmd.exe wrappers,
   ask the user about broader permission allow-list patterns
   BEFORE getting blocked mid-flow. One question saves N mid-
   work interruptions and tool-call retries.

8. **PYTHON FILE I/O: ALWAYS `newline=''`.** Default to
   `io.open(path, 'r'/'w', encoding='utf-8', newline='')`.
   Without it, Python silently translates LF <-> CRLF on
   Windows and breaks EOL conformance in committed code (we
   hit this once already; not a third time).

9. **TRANSFORMATION-SCRIPT PRE-FLIGHT.** Before running ANY
   slice/transform script (Python, sed, awk) over source files:
   - **Endpoints**: verify each slice's `head -3` and `tail -3`
     -- a closing `}` / `return X;` / `;` off-by-one disappears
     silently until first compile of the consuming code.
   - **Bare references**: for each slice that becomes a new
     function with a Context preamble (alias list),
     `grep -oE '\b[a-z_][a-zA-Z0-9_]*\b' slice | sort -u` and
     match against the alias list. Anything in the slice but not
     aliased = missing reference = future build failure.
   - **Sibling-file pre-reads**: when writing a new .cpp next to
     an existing one, READ the existing file's includes /
     namespace / using-declarations first. Don't guess header
     paths (`compose/compose.h` vs the real `compose/frame.h`).
   - **Temp .py on disk is OK** when the script is >50 lines
     and you might need to re-read it for debugging, BUT delete
     it at the end of the stage (cleanup mandate). FORMAT-as-
     you-extract: run `clang-format` on the affected file
     IMMEDIATELY after the script's output, not at the end of
     the refactor.

### D. Communicate

10. **NO TABLES IN STATUS UPDATES.** During work, report in one
    sentence: "Step N done, run() now X lines, tests green."
    Skip the 7-row "what file holds what" tables -- the user
    sees file sizes with `wc -l`. Context window is a shared
    resource. **Exception**: structural before/after delta
    tables at the END of a multi-stage refactor are fine when
    they convey new information (sizes, file split, metric
    drift).

11. **DON'T INGEST UNNECESSARY TEXT.** Three related cases:
    - **After Edit/Write**: don't re-read the file -- the tool
      guarantees the write succeeded (or errored), and the
      system reminder shows post-edit content if noteworthy.
    - **Schema dumps / error expansions**: when a tool returns
      a 500-line schema (settings.json validation, type-deep
      errors), DON'T ingest the full dump. Read 1-2 lines of
      the actual error and infer the fix.
    - **Find by Grep, not by Read**: use `Grep -n -A/-B/-C` for
      symbols / boundaries; Read only when you need >20
      contiguous lines or context above an unknown location.
    All three rules share one principle: every token you read
    is a token you can't spend on thinking.

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

6. **Small, single-purpose units.** Code goes into the smallest
   sensible unit -- a function per logical pass, a `.cpp` per
   logical module, a `.h` for shared internals. Established by the
   2026-05-28 sanitary refactor of `src/rsm/` (commit pending),
   which split a 2158-line `rsm_reader.cpp` with a 1638-line
   `Reader::open()` into 7 .cpp files + 2 .h files, with the
   biggest .cpp at ~625 lines and `Reader::open()` at ~50 lines.
   Concrete guidelines:

   - **Functions: hundreds of lines is a smell, thousands is a
     bug.** When a function tops ~200 lines or grows a second
     distinct phase, extract a named helper. Mega-functions hide
     dataflow and force callers to re-read context every time.
   - **`.cpp` files: one logical responsibility each.** New
     unrelated functionality goes into a new `.cpp`, not appended
     to an existing one. The `src/rsm/rsm_*.cpp` family is the
     reference shape: `rsm_reader.cpp` dispatches, `rsm_primitives
     .cpp` / `rsm_procs_vars.cpp` / `rsm_unit_tables.cpp` /
     `rsm_aggregates.cpp` / `rsm_dump.cpp` / `rsm_decorate.cpp`
     each own one pass. Same pattern in `src/cli/cli_cmd_*.cpp`
     (rule #3 above).
   - **Shared internals: an `_internal.h` header with
     `inline` / `inline constexpr` items in a `detail` namespace.**
     `src/rsm/rsm_internal.h` is the prototype: record-tag
     constants, the primitive descriptor table, byte-level
     helpers (`readU32LE`, `findBytes`, `isPrintableName`).
     Anonymous namespaces in `.cpp` files are fine for things
     used by ONE TU; promote to `*_internal.h` the moment a
     second TU needs them, never copy-paste.
   - **Extraction stays behaviour-identical.** Each extraction is
     a pure refactor: build clean + `rsm2pdb_tests` green + a
     smoke run that prints the same `[rsm] ...` counters. The
     `2026-05-28` refactor sequence (steps 1..5 + the split into
     submodules) is the worked example -- six incremental edits,
     six verification cycles, no regressions.
   - **`open()`-style dispatchers stay tiny.** The top-level
     entry point should read like a table of contents: load
     input, call each phase, return. If you can't tell what a
     function does in one glance, the function has too much
     going on.
   - **Extractions are reversible-by-inlining.** A good helper
     can be re-inlined cleanly because its signature captures
     exactly what it depends on. If extraction needs to thread
     ten parameters or pass a giant captures-everything closure,
     the boundary is in the wrong place -- redesign instead of
     forcing it.

---

## Gotchas (reference; not binding)

See [`docs/06-gotchas.md`](docs/06-gotchas.md) for the full numbered
catalogue (currently #1..#37). Reach for it when debugging in the
matching area; do not memorise. New gotchas: append at the bottom
with the next number, never renumber existing ones.

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

For next-step candidates, current state, deferred items, and key
decisions: read `todo.txt` (DONE section is most-recent-first; TODO
section is numbered; KEY DECISIONS LOG carries D-001..). The polished
design view is in `docs/01-architecture.md` and friends.

For RSM format facts / hypotheses (active RE work): `rsm-format.txt`
(lab notebook, append-only) + `docs/02-rsm-format-notes.md` (polished).

Diagnostic subcommands that exist (invaluable for future RSM RE):
`rsm2pdb diff-procs`, `rsm2pdb probe-procs`, `rsm2pdb analyze-procs`.

**Hard rules** (don't break):
- Reversing the line tables -- D-016 established `.map` is authoritative.
- Using DWARFContext for testing -- see workflow rule #7.
- Broadening allowlisting beyond what `.claude/settings.local.json` already covers.
