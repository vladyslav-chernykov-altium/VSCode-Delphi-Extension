# rsm2pdb-vscode (skeleton)

Internal-use VSCode extension that wraps the rsm2pdb pipeline:
discover a `.dproj` in the workspace, build it via Delphi MSBuild,
inject DWARF, and start a debug session — all from one command.

**Status: skeleton.** Source is in place; commands print
placeholders. Real wiring lands when M2 (variable inspection) is
further along — the extension's UX value depends on it.

## Layout

```
vscode-ext/
  package.json     extension manifest + commands + settings
  tsconfig.json    TS build config
  src/
    extension.ts   commands + skeleton logic
  README.md        this file
```

## Develop

```cmd
cd vscode-ext
npm install
npm run compile
```

Then open `vscode-ext/` in a separate VSCode window and press **F5**
to launch an Extension Development Host with this extension loaded.

The host opens with no folder; use *File > Open Folder* on the parent
`rsm2pdb` workspace (or any Delphi project) to test.

## Commands (current state — most are placeholders)

| Command | What it does today |
|---|---|
| `rsm2pdb: Build Delphi project and start debugging` | Shows pick UI, prints what it WOULD build/launch |
| `rsm2pdb: Build Delphi project (no launch)`        | No-op stub |
| `rsm2pdb: Pick the active .dproj`                  | Lists discovered .dproj files in the OutputChannel |

## Configuration (`settings.json`)

| Key | Default | Purpose |
|---|---|---|
| `rsm2pdb.bdsPath`        | `C:\Dev\Tools\Embarcadero\Studio\19.0` | Delphi (BDS) install path |
| `rsm2pdb.executable`     | `${workspaceFolder}/build/src/rsm2pdb.exe` | Path to rsm2pdb.exe |
| `rsm2pdb.gdbPath`        | `C:/Dev/Tools/msys64/mingw64/bin/gdb.exe` | mingw-w64 gdb |
| `rsm2pdb.defaultPlatform`| `Win64` | Only Win64 supported by pipeline today |

## What's stubbed vs implemented

| Piece | Status |
|---|---|
| `findDelphiProjects()` | Implemented (uses `vscode.workspace.findFiles`) |
| `runBuild()`           | Stub — logs what it would invoke |
| `writeLaunchEntry()`   | Stub — dumps the JSON it would write |
| `vscode.debug.startDebugging()` call | Not wired |
| sourceFileMap from .dproj's DCCReference items | Not implemented |
| DLL projects (host .exe selection) | Not implemented |
| Error parsing from MSBuild output | Not implemented |
| Cancellation / progress UI | Not implemented |

## Why this exists as a skeleton

The user-facing payoff of this extension is highest once rsm2pdb
itself shows records / classes / strings nicely — which requires
the M2 RSM parsing work. The skeleton claims the structure and
the manifest entry points so we can fill it in incrementally
without restructuring later.

For day-to-day use today, prefer `scripts/delphi-debug.ps1` directly
(see the VSCode tasks `delphi-debug-build (hello)` /
`delphi-debug-build (two_units)` in `.vscode/tasks.json`).

## Packaging (later)

```cmd
npx vsce package
```

Produces `rsm2pdb-vscode-0.0.1.vsix` which can be installed via
`Extensions > ... > Install from VSIX`.
