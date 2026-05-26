# rsm2pdb-vscode

VSCode extension that drives the rsm2pdb pipeline for Delphi (Win64)
projects end-to-end: parse `.dproj` → build via Delphi MSBuild → inject
DWARF via `rsm2pdb` → launch under cppdbg + gdb.

## Status

Working today against real Delphi projects (incl. Altium's AdvPCB:
120 MB DLL / 543 MB `.rsm` / 267 MB `.map`).

| Capability | State |
|---|---|
| Discover `.dproj` files in the workspace | ✅ |
| Parse `.dproj` configs (all `(Config, Platform)` tuples) | ✅ |
| Inspect resolved settings (output paths, host EXE, etc.) | ✅ |
| Run incremental build (`/t:Make`) | ✅ |
| Run full rebuild (`/t:Rebuild`) | ✅ |
| Inject DWARF in-place via `rsm2pdb dwarf` | ✅ |
| Stream compiler output live to the Output channel | ✅ |
| Populate PROBLEMS tab with Delphi compiler hints/warnings/errors | ✅ |
| Cancel a running build (whole process tree) | ✅ |
| Launch cppdbg + gdb with proper source-search paths | ✅ |
| Quick relaunch debug of last build without rebuilding | ✅ |

## Commands + keybindings

| Key | Command | What |
|---|---|---|
| `Ctrl+B` | `rsm2pdb.build` | **Make** — incremental build + DWARF inject |
| `Ctrl+Shift+B` | `rsm2pdb.rebuild` | **Full rebuild** + DWARF inject |
| `Ctrl+F9` | `rsm2pdb.buildAndDebug` | Make + Run (launch cppdbg) |
| `Ctrl+Shift+F9` | `rsm2pdb.debugLast` | Launch last build (no rebuild) |
| `Ctrl+Shift+C` | `rsm2pdb.cancelBuild` | Cancel running build (kills tree) |
| _Ctrl+Shift+P_ | `rsm2pdb.inspectProject` | Parse a `.dproj` and dump its configs |
| _Ctrl+Shift+P_ | `rsm2pdb.pickProject` | List discovered `.dproj` files |

`F9` itself stays as VSCode's default (toggle breakpoint).

## Settings

| Setting | Default | Purpose |
|---|---|---|
| `rsm2pdb.bdsPath` | `C:\Dev\Tools\Embarcadero\Studio\19.0` | Delphi install (used to find `bin\rsvars.bat`). |
| `rsm2pdb.executable` | `${workspaceFolder}/build/src/rsm2pdb.exe` | Path to `rsm2pdb.exe`. Auto-falls-back to the extension's sibling `build/src/rsm2pdb.exe` during dev. |
| `rsm2pdb.gdbPath` | `C:/Dev/Tools/msys64/mingw64/bin/gdb.exe` | mingw-w64 gdb used by cppdbg. |
| `rsm2pdb.defaultPlatform` | `Win64` | Reserved; only Win64 is supported by rsm2pdb today. |

## What the pipeline does

```
.dproj ─┐
        ├─► dprojParser ──► (Config, Platform) tuples + resolved settings
        │                   (DCC_ExeOutput, Debugger_HostApplication,
        │                    DCC_UnitSearchPath, DCCReference items, ...)
        │
        ├─► buildRunner ──► rsvars.bat → msbuild ──► .dll + .map + .rsm
        │                                       └─► rsm2pdb dwarf (in-place)
        │
        └─► extension.ts ──► startDebugSession
                              ├─► build -environment-directory list
                              │   from DCC_UnitSearchPath + DCC_IncludePath
                              │   + DCCReference dirs + dproj dir
                              ├─► compose cppdbg launch config
                              │   (program = host EXE for Library projects)
                              └─► vscode.debug.startDebugging(...)
```

## Develop

```cmd
cd vscode-ext
npm install
npm run compile
```

Open `vscode-ext/` in VSCode and press **F5** to launch an Extension
Development Host. In the dev-host window, open any folder containing
a `.dproj` and the extension activates automatically.

`npm run watch` keeps `dist/extension.js` in sync; reload the dev host
with `Ctrl+R` after changes.

## Verifying it works

1. F5 from `vscode-ext/`.
2. In the dev host, open a folder with a Delphi `.dproj`.
3. `Ctrl+Shift+P` → `rsm2pdb: Inspect project` → pick a config.
   Look at the Output panel (`rsm2pdb`) — you should see the resolved
   settings JSON and any warnings about `DCC_DebugInfoInExe` /
   `DCC_MapFile` defaults.
4. `Ctrl+B` → build only. Wait for `Build succeeded -> <path>`.
5. `Ctrl+F9` → build + debug. cppdbg launches the host EXE under gdb.
   Set a breakpoint in any unit; it should hit and display source.
6. `Ctrl+Shift+F9` → relaunch the previous debug session without
   any build steps.

## Known limitations

- Win64 only (Win32 not supported by the rsm2pdb backend yet).
- Type display for primitives merges same-size types (Cardinal shows
  as Integer, Double as Int64, etc.) — see project root's
  [README.md](../README.md) for gdb-side workarounds
  (`-exec print/f gD` etc.).
- The .dproj parser is best-effort regex-based; very unusual
  inheritance structures may not resolve correctly. The
  `rsm2pdb: Inspect project` command surfaces what was resolved so
  any gap is visible.
- `DCC_DebugInfoInExe` defaulted-on `.dproj`s may still hit the
  PE-header-room gotcha when injecting DWARF on big DLLs. Set
  `<DCC_DebugInfoInExe>false</DCC_DebugInfoInExe>` in the .dproj
  to be safe.

See [docs/05-vscode-extension.md](../docs/05-vscode-extension.md) for
the architecture-level write-up.

## Packaging (later)

```cmd
npx vsce package
```

Produces `rsm2pdb-vscode-0.0.1.vsix`, installable via *Extensions →
… → Install from VSIX*.
