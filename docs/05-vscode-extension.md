# VSCode extension architecture

Architecture-level companion to [vscode-ext/README.md](../vscode-ext/README.md).
The README explains *what the extension does*; this doc explains
*how it does it* and *why the pieces are shaped the way they are*.

The extension's job is to glue four already-working things together
into one keystroke:

1. The `.dproj` (Delphi MSBuild project file) as the source of truth
   for build config, output paths, host app, and source search paths.
2. The Delphi compiler / MSBuild as the build engine.
3. Our own `rsm2pdb.exe` as the DWARF injector.
4. VSCode's cppdbg + mingw-w64 gdb as the debug front end.

Three goals shape the design:

- **No build-step manual ceremony.** One keystroke moves from "I
  edited a `.pas` file" to "the debugger is stopped on my
  breakpoint."
- **Zero runtime dependencies beyond what the user already has.** No
  XML parser, no glob library, no spawn-helper. Stays small and
  immune to npm-supply-chain churn.
- **All decisions traceable to the `.dproj`.** The extension never
  invents settings; it parses what Delphi already wrote.

## Module layout

```
vscode-ext/src/
  extension.ts      activate(); command registrations; pickers;
                    startDebugSession (cppdbg launch composition);
                    workspace-state for "last pick".
  dprojParser.ts    listConfigs() + resolveSettings(); regex-based
                    XML reader; env-var expansion; CfgParent
                    inheritance walker.
  buildRunner.ts    runBuild() pipeline: rsvars + msbuild + rsm2pdb
                    dwarf. Diagnostic parser. Cancellation.
```

No other files. Three modules, ~700 lines of TypeScript.

## dprojParser.ts

Delphi's `.dproj` is MSBuild XML. The interesting bits live in
`PropertyGroup` elements gated by conditions like:

```xml
<PropertyGroup Condition="'$(Config)'=='Debug :: Built-In' or '$(Cfg_11)'!=''">
<PropertyGroup Condition="('$(Platform)'=='Win64' and '$(Cfg_11)'=='true') or '$(Cfg_11_Win64)'!=''">
<PropertyGroup Condition="'$(Cfg_11_Win64)'!=''">
```

Each named config (`"Debug :: Built-In"`, `"Release"`, ...) gets
mapped to an internal `Cfg_N` id, and each `(Config, Platform)`
tuple has up to four scopes that overlay in order:

```
Base  →  Base_<Platform>  →  Cfg_N  →  Cfg_N_<Platform>
```

`resolveSettings()` walks that chain, merges raw key/value pairs,
then expands `$(NAME)` env-var references. Output is a flat
`DprojSettings` struct.

### Why regex, not a real XML parser

Delphi's .dproj XML is well-formed but limited in vocabulary — a
handful of `PropertyGroup` shapes plus `<DCCReference>` items. A
focused regex (`<([A-Za-z_][\w.]*?)>([\s\S]*?)</\1>`) for key/value
pairs and a few specific patterns for `Condition` decoding gets us
everything we need in ~150 lines with zero deps. We surface the
parsed settings via `rsm2pdb: Inspect project` so any miss is
visible immediately.

### Key fields exposed to the rest of the extension

```ts
interface DprojSettings {
  appType: 'Library' | 'Application' | 'Package';
  mainSource: string;
  projectName: string;
  outputDir: string;
  outputBaseName: string;
  outputExtension: string;        // .dll for Library, .exe otherwise
  outputFullPath: string;
  hostApplication: string | null;  // for DLL projects
  debuggerCwd: string | null;
  debuggerRunParams: string | null;
  mapFile: number;
  debugInformation: number;
  localDebugSymbols: boolean;
  debugInfoInExe: boolean;
  optimize: boolean;
  remoteDebug: boolean;
  useMSBuildExternally: boolean;
  define: string;
  unitSearchPaths: string[];       // DCC_UnitSearchPath, resolved abs
  includeSearchPaths: string[];    // DCC_IncludePath, resolved abs
  dccReferences: string[];         // <DCCReference Include=...> items
}
```

`unitSearchPaths[0]` is always the .dproj's own directory.

## buildRunner.ts

The build pipeline is two child-process invocations bridged by
file-system handoff:

```
              ┌── tools.rsvarsBat ──┐
              ▼                     ▼
        msbuild   /t:<Make|Build|Rebuild>
                  /p:Config=...
                  /p:Platform=...
              ▼
         <dll/exe>  + .map  + .rsm
              ▼
        rsm2pdb.exe  dwarf  <map>  <input>  <output (same)>
              ▼
       DWARF-enriched DLL/EXE, in place
```

### Quoting / spawn

Node's `child_process.spawn` on Windows has aggressive quote-escaping
for `cmd.exe` arguments. To avoid that, both phases are emitted as a
temp `.bat` file and run as `cmd /c <tmp.bat>`. The wrapper is
trivial and looks like:

```bat
@echo off
call "C:\Dev\Tools\Embarcadero\Studio\19.0\bin\rsvars.bat" || exit /b 1
msbuild "AdvPCB.dproj" ^
  /t:Make ^
  /p:Config="Debug :: Built-In" ^
  /p:Platform=Win64 ^
  /nologo /v:normal /clp:Summary;NoItemAndPropertyList
```

`/v:normal` is the verbosity sweet spot — `/v:minimal` is silent
on incremental builds (looks frozen), `/v:detailed` is too noisy.

### Cancellation

A module-level `currentChild: ChildProcess | undefined` tracks the
in-flight spawn. `cancelBuild()` runs `taskkill /pid <pid> /T /F`
which kills the whole tree — Delphi's `dcc64.exe` runs as a
grandchild of `cmd.exe`, so killing just the parent leaves it
running. The `/T` flag is essential.

### Diagnostic parser

dcc64 emits compiler hints/warnings/errors interleaved with progress
output:

```
Datamodel\...\NETOBJ_impl.PAS(56): Hint warning H2269: Overriding virtual method ...
Foo.pas(123,5) Error: E2010 Incompatible types
[dcc64 Warning] Bar.pas(99): W1011 ...
```

A line-buffered regex picks these up as they stream past, groups
them by file URI (resolved against the .dproj directory if
relative), and the build runner publishes them to a
`vscode.DiagnosticCollection` so they show in the PROBLEMS tab.

Severity mapping (note Hint → **Information**, not `Hint`):

| Delphi tag | VSCode severity | Why |
|---|---|---|
| `Hint` / `Hint warning` | `Information` | `Hint` severity doesn't render as a PROBLEMS row, just inline editor decorations. Information shows. |
| `Warning` | `Warning` | |
| `Error` / `Fatal Error` | `Error` | |

## extension.ts

### Pickers

`pickProject()` runs `vscode.workspace.findFiles('**/*.dproj')` and
auto-picks if there's only one. `pickConfig()` lists every
`(Config, Platform)` tuple from `dprojParser.listConfigs()`.

### startDebugSession

The cppdbg launch config is composed dynamically per run, not stored
in `.vscode/launch.json`. Composition handles three things:

1. **`program`.** For `AppType=Library`, this is
   `Debugger_HostApplication` from the .dproj (e.g. the host EXE
   that will load the DLL). For `Application`, it's the
   `outputFullPath`. If the host EXE field is missing for a Library
   project, we abort with a clear message in the Output channel.

2. **Source search paths via `-environment-directory`.** gdb's
   native source-finding mechanism. We collect:
   - The .dproj's own directory.
   - Each `DCC_UnitSearchPath` entry (resolved abs).
   - Each `DCC_IncludePath` entry (resolved abs).
   - Each `<DCCReference Include=...>` file's parent directory.

   For each existing directory we emit a setup command:

   ```
   -environment-directory "C:/Dev/Src/X2/.../Source code"
   ```

   gdb then resolves any bare DW_AT_name file (our DWARF emits
   `"AdvPCB.dpr"` with empty comp_dir) by walking this list. cppdbg
   gets a resolved absolute path back from gdb and opens it.

3. **`-gdb-set case-sensitive off`.** Pascal is case-insensitive;
   gdb defaults to case-sensitive even for Pascal language. This
   setupCommand makes `gd`, `GD`, `Gd` all resolve to `gD` in
   Watch / Debug Console. (Cosmetic warning at session start
   about "case sensitivity does not match language" — ignored.)

### Workspace state — "last pick"

Each build command (`build`, `rebuild`, `buildAndDebug`) writes the
chosen `(dprojPath, cfgId, cfgName, platform)` tuple into
`context.workspaceState` under `'rsm2pdb.lastPick'`. The
`debugLast` command (Ctrl+Shift+F9) reads that back, skips the
build steps entirely, and goes straight to `startDebugSession`.
Falls back to a picker if no last-pick is recorded.

## Keybindings

Encoded in `package.json contributes.keybindings`:

```jsonc
[
  { "command": "rsm2pdb.build",         "key": "ctrl+b",
    "when": "workspaceFolderCount > 0 && !inputFocus" },
  { "command": "rsm2pdb.rebuild",       "key": "ctrl+shift+b",
    "when": "workspaceFolderCount > 0" },
  { "command": "rsm2pdb.debugLast",     "key": "ctrl+f9",
    "when": "workspaceFolderCount > 0" },
  { "command": "rsm2pdb.cancelBuild",   "key": "ctrl+f2",
    "when": "workspaceFolderCount > 0" },
  { "command": "rsm2pdb.buildAndDebug", "key": "ctrl+shift+f9",
    "when": "workspaceFolderCount > 0" }
]
```

`Ctrl+B` carries the `!inputFocus` guard so the editor's normal
typing of `Ctrl+B` still works inside text inputs. `Ctrl+F2` was
picked for Cancel because the obvious `Ctrl+Shift+C` collides with
VSCode's `Terminal: Open New External Terminal`. `F9` itself stays
free for breakpoint-toggle; build+run uses `Ctrl+Shift+F9` and
relaunch-without-build uses `Ctrl+F9`.

## Status-bar UI

A horizontal cluster of pills sits in the bottom-left of the status
bar whenever a `.dproj` is present in the workspace (priorities are
descending left-to-right):

```
[📦 ProjectName] [⚙ Debug Win64] [🔧 Build] [🔄 Rebuild]
                                 [▶ Run] [⟳ Make+Run] [⊘ Cancel]
```

- **`📦 Project pill`** -- shows the active `.dproj`'s base name.
  Click to swap (`rsm2pdb.pickActiveProject`).
- **`⚙ Config pill`** -- shows the active (Config name, Platform)
  tuple. Click to swap (`rsm2pdb.pickActiveConfig`) -- lists the
  configurations of the currently-active project.
- **`🔧 Build`** -- `rsm2pdb.build` (Make incremental, no launch).
- **`🔄 Rebuild`** -- `rsm2pdb.rebuild` (full clean rebuild, no
  launch).
- **`▶ Run`** -- `rsm2pdb.debugLast` (launch last build, no build
  step).
- **`⟳ Make+Run`** -- `rsm2pdb.buildAndDebug` (Make + launch).
- **`⊘ Cancel`** -- `rsm2pdb.cancelBuild`. Background flips to
  `statusBarItem.warningBackground` (orange/yellow) while a build
  is in progress so the kill switch is visually impossible to miss.

The active-pick state is persisted in `context.workspaceState` under
`'rsm2pdb.lastPick'` so it survives reloads. Build / Rebuild /
Make+Run read the saved pick without re-prompting; the first invocation
in a clean workspace falls back to the picker flow and saves the
choice.

## "View as ..." context menu

Right-click a variable in the Variables panel (or an entry in the
Watch panel) and the `rsm2pdb: View as ...` submenu offers
on-the-fly reinterpretation:

```
View as String (UnicodeString)        -> (wchar_t*)(var)
View as AnsiString (char*)            -> (char*)(var)
View as WideString (wchar_t*)         -> (wchar_t*)(var)
View as Integer (Int32)               -> *(int*)&(var)
View as Cardinal (UInt32)             -> *(unsigned int*)&(var)
View as Int64                         -> *(__int64*)&(var)
View as UInt64                        -> *(unsigned __int64*)&(var)
View as Byte                          -> *(unsigned char*)&(var)
View as Word (UInt16)                 -> *(unsigned short*)&(var)
View as Single (float)                -> *(float*)&(var)
View as Double                        -> *(double*)&(var)
View as Byte[16] (hex dump)           -> *(unsigned char(*)[16])&(var)
View as Byte[64] (hex dump)           -> *(unsigned char(*)[64])&(var)
```

The chosen expression is evaluated against the current stack frame
via DAP `evaluate`. The result lands in two places: a toast
notification (immediate feedback) and the Debug Console (so the user
has a scrollable history, can re-evaluate by editing).

Useful when a variable comes through as `void*` / native-int because
the per-unit type table didn't carry enough info -- the user can
peek through to "what would this look like if it were a wide-char
string". Equivalent to manually adding `(wchar_t*)var` to Watch but
one click instead of typing.

## Skip-stops-in-source-less code

Configured by `rsm2pdb.skipUnknownSourceMode` (default `"out"`):

- `"out"` -- after each step that lands in a frame whose source file
  we can't locate on disk, issue `stepOut` until we surface in
  source-bearing code. Budget 30 attempts. This is the safe default:
  `stepOut` sets a temp BP at the return address, so it can't bypass
  a user breakpoint sitting in the same function.
- `"hybrid"` -- try `stepIn` first (50 attempts), then fall back to
  `stepOut` (30). Catches the case where RTL code calls a user
  callback. **Warning**: `stepIn` chains run through user code that
  may contain breakpoints, and some debug adapters report such hits
  as `reason: 'step'` -- we mitigate via the `hitBreakpointIds`
  check below, but the safer `"out"` default exists for a reason.
- `"off"` -- disable; debug as-is.

Safety:
- Triggers only on `reason: 'step'` stops -- breakpoint / pause /
  exception / entry stops are honoured verbatim.
- Aborts immediately if the `stopped` event carries
  `hitBreakpointIds` (user BP was actually hit, debugger
  miscategorised the reason).
- Per-thread / per-session budget; resets on `terminated` /
  thread change.

## RTL / framework symbol stripping

Driven by `rsm2pdb.includeRtlSymbols` (default `false`). When the
setting is `false`, `rsm2pdb pdb` is invoked without
`--include-rtl`, and the converter drops every public + module
whose qualified Pascal name starts with one of these prefixes:

```
System  SysInit  Winapi  Vcl  Fmx  Soap  Datasnap  Data
Inet    IBX      REST    Bde  IndySystem
```

On examples/05_types the savings are dramatic:

```
publics: 40 emitted (was 2386 -- 2346 RTL-filtered)
modules: 1 (RTL-filtered 12)
PDB size: ~60 KB (was ~250 KB+)
```

Real-world projects scale linearly: an AdvPCB-sized PDB drops from
hundreds of MB to tens. Step Into into a stripped RTL function
lands at an offset with no symbol, our auto-step-skip kicks in
(source-path is empty + symbol name is empty) and we Step Out
straight back to the user-code caller.

Flip the setting to `true` if you really want RTL stepping; just
make sure the Embarcadero install's `source/rtl/*.pas` is on
`--src-search` (configured via the .dproj's unit / include search
paths or the project's `DCCReference` list), otherwise the
debugger will still complain about missing source.

## Up-to-date check

Before each `rsm2pdb pdb` / `rsm2pdb dwarf` invocation, the build
runner compares output mtime against (`.exe`, `.map`, `.rsm`,
`rsm2pdb.exe`) mtimes. If output is newer than every input the step
is skipped with a `[skip rsm2pdb pdb: name.pdb is up to date]`
line in the Output channel.

- PDB backend: the `.pdb` file itself is the stamp.
- DWARF backend: a sibling `.<base>.dwarf-stamp` file is touched
  after each successful inject (because DWARF mutates the exe
  in-place, so the exe can't be its own stamp).

This makes "Run" (debugLast) and "Make+Run" cheap when nothing
changed: msbuild finishes its no-op Make in ~0.5s and rsm2pdb is
skipped entirely.

## Activation

```jsonc
"activationEvents": [
  "workspaceContains:**/*.dproj"
]
```

The extension only loads when a `.dproj` is present anywhere in
the workspace. No background activity in other workspaces.

## Things that are deliberately NOT here

- A real XML parser. The regex approach handles every .dproj shape
  we've seen, with zero dependencies.
- Progress-notification widgets. The Output channel + status-bar
  pills + Problems panel cover the user-facing surface.
- Persistent `.vscode/launch.json` entries. Composed dynamically.
- Workspace-scope settings UI. Use VSCode's regular settings
  editor; the few `rsm2pdb.*` keys cover the configurable surface
  (`bdsPath`, `executable`, `gdbPath`, `backend`, `console`,
  `skipUnknownSourceMode`).

## Known limitations

Same list as the README; the relevant architectural notes:

- Single-workspace assumption. Multi-root workspaces aren't tested.
  The first workspace folder is used for `${workspaceFolder}`
  substitution and for the "last pick" state key.
- Diagnostic parsing is regex-based against a few observed dcc64
  output shapes. Unknown shapes are silently ignored (they still
  appear in the Output channel — just not in PROBLEMS).
- Cancellation uses Windows-only `taskkill /T /F`. POSIX support
  would need a different process-tree-kill strategy.

## Future work

- Wire the extension's pipeline to publish progress via
  `vscode.window.withProgress` so cancel becomes a UI button too.
- Watch the .dproj for changes and refresh the config list.
- Surface mismatch warnings in the resolved settings (e.g.,
  `DCC_DebugInfoInExe=true` for a DLL injection target) as a
  CodeLens or notification, not just an Output line.
- Allow per-workspace overrides of the resolved settings via a
  `.vscode/settings.json` `rsm2pdb.overrides` block.
