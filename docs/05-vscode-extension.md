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
  { "command": "rsm2pdb.buildAndDebug", "key": "ctrl+f9",
    "when": "workspaceFolderCount > 0" },
  { "command": "rsm2pdb.cancelBuild",   "key": "ctrl+shift+c",
    "when": "workspaceFolderCount > 0" },
  { "command": "rsm2pdb.debugLast",     "key": "ctrl+shift+f9",
    "when": "workspaceFolderCount > 0" }
]
```

`Ctrl+B` carries the `!inputFocus` guard so the editor's normal
typing of `Ctrl+B` still works inside text inputs. Other keys are
unconditional because their conflicts (`Ctrl+Shift+B` = VSCode's
Run Build Task, `F9` = toggle breakpoint) are exactly what we want
to override (Ctrl+Shift+B) or NOT want to override (we use Ctrl+F9
and Ctrl+Shift+F9 instead of plain F9 specifically to preserve
breakpoint-toggle).

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
- A status-bar item or progress notifications. The Output channel
  is enough; bells and whistles can come later.
- Persistent `.vscode/launch.json` entries. Composed dynamically.
- Workspace-scope settings UI. Use VSCode's regular settings
  editor; the four `rsm2pdb.*` keys cover the configurable surface.

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
