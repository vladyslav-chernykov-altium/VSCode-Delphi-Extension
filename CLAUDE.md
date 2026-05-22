# CLAUDE.md — orientation for future sessions

If you're a new Claude (or human) picking this project up, **read this file
first**, then `todo.txt`, then `rsm-format.txt`. Everything else follows.

---

## Project at a glance

**rsm2pdb** converts Delphi's `.map` (and eventually `.rsm`) debug-info
files into DWARF and injects the DWARF into the matching PE, so VSCode +
gdb can debug Delphi Win64 binaries.

Status snapshot (auto-rotting; verify against the latest commit):

| Milestone | State |
|---|---|
| M1 — source-level line debugging | ✅ done, user-verified in VSCode |
| M2 phase C — opaque global variables (no types yet) | ✅ done |
| M2 phase B — type records from RSM | ⏳ not started; first concrete next step |
| M2 phase A — locals / parameters / classes / strings | ⏳ later |
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
| `docs/03-dwarf-emission-notes.md` | DIE shapes, type map, PE-injection mechanics. |
| `docs/04-validation-plan.md` | What each milestone validates and how. |
| `README.md` | User-facing capabilities + "how to use on your own Delphi project". |
| `src/map/` | Delphi .map text parser. |
| `src/dwarf/` | DWARF v5 emitter (hand-rolled, LLVM for constants only). |
| `src/pe/` | PE section injector. |
| `src/model/` | Debugger-agnostic intermediate IR. |
| `src/rsm/` | **Stub.** This is M2-phase-B work — parse the binary `.rsm` to get types. |
| `examples/01_hello/` | Single-file Delphi sample. `build.cmd` runs Delphi MSBuild + `rsm2pdb dwarf`. |
| `examples/02_two_units/` | Multi-unit (Geometry + App.Colors + dpr). Same build pattern. |
| `test/fixtures/` | Committed .exe/.rsm/.map sample inputs. Both fixtures included. |
| `spike/` | History of the de-risking spikes (6a / 6b / 6c). Not compiled. See `spike/README.md`. |
| `scripts/install-deps.ps1` | One-time installer for MSYS2 + LLVM. |
| `scripts/delphi-debug.ps1` | Convenience wrapper: build + inject for a given .dproj. |
| `vscode-ext/` | TypeScript extension SKELETON. Not wired today. |
| `.vscode/` | tasks.json + launch.json + settings.json for VSCode workflow. |

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
6. **Tests-first style is welcome but pragmatic.** doctest unit
   tests for parsers; byte-level structural tests for the DWARF
   emitter (LLVM's DWARFContext as a reader proved finicky for in-
   memory sections — gave up after ~20 min, switched to byte-level
   asserts).
7. **Verbosity preference:** show what's happening at key phase
   boundaries; don't dump every compile command. Long-running
   commands belong in `run_in_background` with notification on
   completion.
8. **The user auto-approves obvious read-only operations**
   (`ls`, `grep`, `od`, hex dumps, gdb in batch mode). Don't ask
   permission for these. Do ask before writing files outside the
   workspace, deleting things, or running long builds.
9. **Bash on Windows uses Unix syntax** but the workspace is at
   `c:/Dev/Src/rsm2pdb/` with forward slashes. Bash is fine for
   simple stuff; for CMake builds we go through `cmd.exe /C` with
   `vcvars64.bat` because that's how MSVC's environment works.
10. **`scripts/check-vs.cmd` was deleted** — don't recreate it.

---

## Gotchas we hit (worth knowing, may bite again)

1. **Windows Defender / corporate EDR blocks post-link-modified PEs.**
   `objcopy --strip-debug foo.exe` followed by running `foo.exe` gets
   "Access is denied." Even local user-level Defender exclusions are
   overridden by corporate EDR on this machine. **Fix:** integrate
   DWARF injection into the build pipeline so the .exe is "born" with
   DWARF instead of being patched post-hoc. See
   `examples/*/build.cmd` for the pattern.

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
    spirit but not directly applicable. The RSM parser will be largely
    reverse-engineered from scratch. See `rsm-format.txt`.

11. **`rsm2pdb dwarf` requires Delphi to NOT embed TD32 in the EXE**
    (set `DCC_DebugInfoInExe=false`). Otherwise the EXE has a 1+ MB
    `.debug` section that uses up PE header slots, and the injector
    fails with "not enough header space". `examples/*/two_units.dproj`
    has this set correctly.

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

**Next step:** RSM P1 — header + early structural blocks of the
`.rsm` file.

Concrete starting move:

```bash
cd test/fixtures
/c/Dev/Tools/msys64/usr/bin/od -An -tx1 -v -N 256 hello.rsm     > /tmp/h.hex
/c/Dev/Tools/msys64/usr/bin/od -An -tx1 -v -N 256 two_units.rsm > /tmp/t.hex
diff /tmp/h.hex /tmp/t.hex
```

Identical bytes are structural (format constants); differing ones are
data. Cross-reference findings with the `[FACT]`/`[GUESS]` entries
already in `rsm-format.txt` (see lines about offsets 0x04, 0x08, 0x0C,
0x10, 0x14, 0x18, 0x20).

After the differential analysis:
1. Write `src/rsm/rsm_reader.{h,cpp}` to parse the header proper
   (magic + the 7 uint32 fields after, EXE path string).
2. Parse the library-search-paths block at offset ~0x426 (we know
   its rough shape but not exact field layout).
3. Update `rsm2pdb dump <foo.rsm>` to show what we parsed.
4. Add doctests.
5. Append findings to `rsm-format.txt` as you go.

**Don't:**
- Try to parse the WHOLE .rsm in P1. P1 = header + early blocks only.
- Reverse the line tables — we already established (D-016) that
  `.map` is authoritative for those. RSM is only for types and
  variable locations.
- Use DWARFContext for testing — see gotcha #2-ish in our session
  history. Byte-level structural tests work fine.

---

*Last updated: end of session that produced commit 434f6ea.*
