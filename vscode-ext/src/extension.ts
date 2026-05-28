// rsm2pdb-vscode  —  Delphi build + DWARF inject + debug launch.
//
// Current capabilities:
//   - rsm2pdb.pickProject     discover .dproj files in the workspace
//   - rsm2pdb.inspectProject  parse a .dproj and dump its build configs
//                              (uses dprojParser.ts; no build / debug yet)
//   - rsm2pdb.buildAndDebug   stub for the full pipeline
//   - rsm2pdb.build           stub for build-only
//
// Build with:
//   cd vscode-ext && npm install && npm run compile
// Then F5 from vscode-ext/ to launch an Extension Development Host.

import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

import {
  listConfigs,
  resolveSettings,
  type DprojConfig,
  type Platform,
} from './dprojParser';
import {
  runBuild,
  cancelBuild,
  isBuildRunning,
  onBuildStateChange,
} from './buildRunner';

interface DelphiProject {
  dprojPath: string;
  projectName: string;
}

async function findDelphiProjects(): Promise<DelphiProject[]> {
  const found = await vscode.workspace.findFiles(
    '**/*.dproj',
    '**/node_modules/**',
    50,
  );
  return found.map((u) => ({
    dprojPath: u.fsPath,
    projectName: path.basename(u.fsPath, '.dproj'),
  }));
}

/** Let the user pick a .dproj from the workspace. */
async function pickProject(
  output: vscode.OutputChannel,
): Promise<DelphiProject | undefined> {
  const projects = await findDelphiProjects();
  if (projects.length === 0) {
    vscode.window.showErrorMessage('rsm2pdb: no .dproj found in workspace');
    return undefined;
  }
  if (projects.length === 1) {
    output.appendLine(`Auto-picked sole project: ${projects[0].projectName}`);
    return projects[0];
  }
  const choice = await vscode.window.showQuickPick(
    projects.map((p) => ({ label: p.projectName, description: p.dprojPath, project: p })),
    { placeHolder: 'Pick a Delphi project' },
  );
  return choice?.project;
}

/** Let the user pick one of the (Config, Platform) tuples from a .dproj. */
async function pickConfig(
  dprojPath: string,
): Promise<DprojConfig | undefined> {
  const configs = listConfigs(dprojPath);
  if (configs.length === 0) {
    vscode.window.showErrorMessage(
      `rsm2pdb: no (Config, Platform) tuples found in ${path.basename(dprojPath)}`,
    );
    return undefined;
  }
  const items = configs.map((c) => ({
    label: `${c.name}  [${c.platform}]`,
    description: c.cfgId,
    config: c,
  }));
  const choice = await vscode.window.showQuickPick(items, {
    placeHolder: 'Pick a build config + platform',
  });
  return choice?.config;
}

export function activate(context: vscode.ExtensionContext): void {
  const output = vscode.window.createOutputChannel('rsm2pdb');
  context.subscriptions.push(output);

  // VSCode rejects gutter-click breakpoints on .pas/.dpr because Pascal
  // isn't in its known-languages list. Flip the workspace-scoped setting
  // so the user doesn't have to discover it manually.
  void (async () => {
    const cfg = vscode.workspace.getConfiguration('debug');
    if (cfg.get<boolean>('allowBreakpointsEverywhere') !== true) {
      try {
        await cfg.update(
          'allowBreakpointsEverywhere',
          true,
          vscode.ConfigurationTarget.Workspace,
        );
        output.appendLine(
          'rsm2pdb: enabled debug.allowBreakpointsEverywhere ' +
            '(needed for .pas/.dpr breakpoints).',
        );
      } catch (e: any) {
        output.appendLine(
          `rsm2pdb: could not set debug.allowBreakpointsEverywhere: ` +
            `${e?.message ?? String(e)}`,
        );
      }
    }
  })();

  // PROBLEMS-tab integration. Populated from Delphi compiler messages
  // captured during msbuild output streaming. Cleared at the start of
  // each build.
  const diagnostics = vscode.languages.createDiagnosticCollection('rsm2pdb');
  context.subscriptions.push(diagnostics);

  // Persist the last-picked (project, config, platform) so that
  // "Debug Last" can skip the build steps and just relaunch.
  interface LastPick {
    dprojPath: string;
    cfgId: string;
    cfgName: string;
    platform: Platform;
  }
  const kLastPickKey = 'rsm2pdb.lastPick';
  const rememberPick = async (
    project: DelphiProject,
    chosen: DprojConfig,
  ): Promise<void> => {
    const v: LastPick = {
      dprojPath: project.dprojPath,
      cfgId: chosen.cfgId,
      cfgName: chosen.name,
      platform: chosen.platform,
    };
    await context.workspaceState.update(kLastPickKey, v);
    refreshActivePickStatusBar();
  };

  // -------- Active-pick model --------
  //
  // The "active pick" is the (project, config, platform) tuple that
  // Build / Rebuild / Make+Run will run against without re-prompting.
  // It's persisted in workspaceState under kLastPickKey, so it
  // survives reloads.
  //
  // Resolution rules:
  //   - If a valid pick is stored (the .dproj still exists, the
  //     config is still listed), reuse it -- no prompt.
  //   - If `force` is true, always prompt and overwrite.
  //   - Otherwise prompt, save, return.
  //
  // The status-bar item below mirrors the pick so the user can see
  // and change it at a glance.

  // Forward declaration -- the actual status-bar item + refresher
  // are created later in activate() once we know `output` etc.
  // are wired up. rememberPick() above closes over this binding.
  let refreshActivePickStatusBar: () => void = () => {};

  interface ActivePick {
    project: DelphiProject;
    config: DprojConfig;
  }
  async function getActivePick(
    force: boolean,
  ): Promise<ActivePick | undefined> {
    const last = context.workspaceState.get<LastPick>(kLastPickKey);
    const fsMod = await import('fs');
    if (!force && last && fsMod.existsSync(last.dprojPath)) {
      // Verify the saved config is still present in the .dproj.
      try {
        const cfgs = listConfigs(last.dprojPath);
        const hit = cfgs.find(
          (c) => c.cfgId === last.cfgId && c.platform === last.platform,
        );
        if (hit) {
          return {
            project: {
              dprojPath: last.dprojPath,
              projectName: path.basename(last.dprojPath, '.dproj'),
            },
            config: hit,
          };
        }
      } catch {
        // .dproj parse failed -- fall through to re-pick.
      }
    }
    const project = await pickProject(output);
    if (!project) return undefined;
    const chosen = await pickConfig(project.dprojPath);
    if (!chosen) return undefined;
    await rememberPick(project, chosen);
    return { project, config: chosen };
  }

  // -------- rsm2pdb.pickProject --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.pickProject', async () => {
      output.show(true);
      const projects = await findDelphiProjects();
      output.appendLine(`rsm2pdb: found ${projects.length} .dproj file(s)`);
      for (const p of projects) {
        output.appendLine(`  ${p.projectName}  (${p.dprojPath})`);
      }
    }),
  );

  // -------- rsm2pdb.inspectProject --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.inspectProject', async () => {
      output.show(true);
      const project = await pickProject(output);
      if (!project) return;
      output.appendLine(`\n=== ${project.projectName} ===`);
      output.appendLine(`Path: ${project.dprojPath}\n`);

      let configs: DprojConfig[];
      try {
        configs = listConfigs(project.dprojPath);
      } catch (e: any) {
        output.appendLine(`error: ${e?.message ?? String(e)}`);
        return;
      }
      output.appendLine(`Configurations (${configs.length}):`);
      for (const c of configs) {
        output.appendLine(`  - ${c.name}  [${c.platform}]  (${c.cfgId})`);
      }

      // Offer to resolve one config's full settings.
      const chosen = await pickConfig(project.dprojPath);
      if (!chosen) return;
      const settings = resolveSettings(
        project.dprojPath,
        chosen.cfgId,
        chosen.platform,
      );
      output.appendLine(`\nResolved settings for "${chosen.name}" [${chosen.platform}]:`);
      output.appendLine(JSON.stringify(settings, null, 2));

      // Flag known gotchas.
      if (settings.debugInfoInExe) {
        output.appendLine(
          `\n[!] DCC_DebugInfoInExe is ON (or defaulted). For best results ` +
            `with rsm2pdb, set <DCC_DebugInfoInExe>false</DCC_DebugInfoInExe> ` +
            `in the .dproj to free PE header room for DWARF section injection.`,
        );
      }
      if (settings.mapFile < 3) {
        output.appendLine(
          `[!] DCC_MapFile = ${settings.mapFile}. rsm2pdb needs detailed maps; ` +
            `set <DCC_MapFile>3</DCC_MapFile> in the .dproj.`,
        );
      }
      if (!settings.hostApplication && settings.appType === 'Library') {
        output.appendLine(
          `[!] AppType is Library but no Debugger_HostApplication set; ` +
            `you'll need to supply one to launch a debug session.`,
        );
      }
    }),
  );

  // -------- rsm2pdb.cancelBuild --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.cancelBuild', () => {
      cancelBuild(output);
    }),
  );

  // -------- rsm2pdb.build (incremental "make") --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.build', async () => {
      output.show(true);
      const pick = await getActivePick(false);
      if (!pick) return;
      const settings = resolveSettings(
        pick.project.dprojPath, pick.config.cfgId, pick.config.platform,
      );
      await runBuild(output, {
        dprojPath: pick.project.dprojPath,
        configName: pick.config.name,
        platform: pick.config.platform,
        settings,
        target: 'Make',
        extensionPath: context.extensionPath,
        diagnostics,
      });
    }),
  );

  // -------- rsm2pdb.rebuild (full rebuild) --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.rebuild', async () => {
      output.show(true);
      const pick = await getActivePick(false);
      if (!pick) return;
      const settings = resolveSettings(
        pick.project.dprojPath, pick.config.cfgId, pick.config.platform,
      );
      await runBuild(output, {
        dprojPath: pick.project.dprojPath,
        configName: pick.config.name,
        platform: pick.config.platform,
        settings,
        target: 'Rebuild',
        extensionPath: context.extensionPath,
        diagnostics,
      });
    }),
  );

  // -------- rsm2pdb.pickActive (force re-pick of BOTH) --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.pickActive', async () => {
      await getActivePick(true);
    }),
  );

  // -------- rsm2pdb.pickActiveProject (just the .dproj) --------
  //
  // Lets the user swap the active project without re-picking the
  // config. If the new project doesn't have a matching (cfg, platform)
  // tuple to whatever was saved, we pick the first one as a sensible
  // default so the user can immediately start building.
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.pickActiveProject', async () => {
      const project = await pickProject(output);
      if (!project) return;
      const configs = listConfigs(project.dprojPath);
      if (configs.length === 0) {
        vscode.window.showErrorMessage(
          `rsm2pdb: no configurations in ${path.basename(project.dprojPath)}`,
        );
        return;
      }
      const last = context.workspaceState.get<LastPick>(kLastPickKey);
      const match = last && configs.find(
        (c) => c.cfgId === last.cfgId && c.platform === last.platform,
      );
      const chosen = match ?? configs[0];
      await rememberPick(project, chosen);
    }),
  );

  // -------- rsm2pdb.pickActiveConfig (just the config within
  //          the current project) --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.pickActiveConfig', async () => {
      const last = context.workspaceState.get<LastPick>(kLastPickKey);
      let dprojPath = last?.dprojPath;
      let projectName = dprojPath
        ? path.basename(dprojPath, '.dproj')
        : '';
      // No project yet -- prompt for one first.
      if (!dprojPath || !(await import('fs')).existsSync(dprojPath)) {
        const project = await pickProject(output);
        if (!project) return;
        dprojPath = project.dprojPath;
        projectName = project.projectName;
      }
      const chosen = await pickConfig(dprojPath);
      if (!chosen) return;
      await rememberPick(
        { dprojPath, projectName },
        chosen,
      );
    }),
  );

  // -------- helpers for buildAndDebug --------
  /**
   * Collect source-search directories from the .dproj:
   *   - The .dproj's own directory (where MainSource lives)
   *   - <DCCReference Include="..."/>  : parent dirs of referenced files
   *   - DCC_UnitSearchPath             : explicit unit dirs
   *   - DCC_IncludePath                : .inc file dirs
   * gdb's `dir <path>` is then used at session start so basename
   * resolutions in the DWARF (`AdvPCB.dpr`, `Geometry.pas`, ...)
   * find a real file in one of these directories. cppdbg / VSCode
   * just opens whatever gdb reports.
   */
  async function buildSourceSearchPaths(
    settings: import('./dprojParser').DprojSettings,
  ): Promise<string[]> {
    const fs = await import('fs');
    const seen = new Set<string>();
    const out: string[] = [];

    const add = (raw: string) => {
      const norm = path.normalize(raw);
      if (seen.has(norm)) return;
      seen.add(norm);
      try {
        if (!fs.existsSync(norm) || !fs.statSync(norm).isDirectory()) return;
      } catch { return; }
      out.push(norm);
    };

    // 1. dproj dir (covers most projects' direct sources).
    add(settings.unitSearchPaths[0] /* prepended by parser */);

    // 2. Search paths (unit + include).
    for (const d of settings.unitSearchPaths.slice(1)) add(d);
    for (const d of settings.includeSearchPaths) add(d);

    // 3. Parent dirs of project-explicit DCCReference files (catches
    //    units that live outside any registered search path).
    for (const fp of settings.dccReferences) {
      try {
        if (fs.existsSync(fp)) add(path.dirname(fp));
      } catch { /* skip */ }
    }

    return out;
  }

  /**
   * Compose a cppdbg launch config for the given project + config +
   * resolved settings, then call vscode.debug.startDebugging. Shared
   * between rsm2pdb.buildAndDebug (after the build runs) and
   * rsm2pdb.debugLast (skip-build relaunch).
   */
  const startDebugSession = async (
    settings: import('./dprojParser').DprojSettings,
    chosen: DprojConfig,
  ): Promise<void> => {
    const gdbPath =
      vscode.workspace.getConfiguration('rsm2pdb').get<string>('gdbPath') ?? '';
    const program = settings.appType === 'Library'
      ? (settings.hostApplication ?? '')
      : settings.outputFullPath;
    if (!program) {
      output.appendLine(
        `error: cannot determine debug program path. For Library projects, ` +
          `the .dproj must set <Debugger_HostApplication>.`,
      );
      return;
    }

    output.appendLine(`Resolving source-search paths from .dproj...`);
    const searchPaths = await buildSourceSearchPaths(settings);
    output.appendLine(`Resolved ${searchPaths.length} source search directories.`);

    // Build BOTH:
    //   1. A sourceFileMap (basename -> abs path) for cppdbg's own
    //      path-remap layer. This is what works reliably for our
    //      example projects' .vscode/launch.json.
    //   2. setupCommands that issue `directory <path>` to gdb so
    //      gdb's source search list is also populated. Belt-and-
    //      braces -- cppdbg may use either depending on whether
    //      gdb's `fullname` field is populated in the stop event.
    const fs = await import('fs');
    const sourceFileMap: Record<string, string> = {};
    const sourceExts = new Set(['.pas', '.dpr', '.inc']);
    for (const dir of searchPaths) {
      try {
        for (const name of fs.readdirSync(dir)) {
          const ext = path.extname(name).toLowerCase();
          if (!sourceExts.has(ext)) continue;
          if (!(name in sourceFileMap)) {
            sourceFileMap[name] = path.join(dir, name);
          }
        }
      } catch { /* skip unreadable dirs */ }
    }
    // Also explicit DCCReference targets (handles files outside any
    // search path).
    for (const fp of settings.dccReferences) {
      try {
        if (fs.existsSync(fp)) {
          const base = path.basename(fp);
          if (!(base in sourceFileMap)) sourceFileMap[base] = fp;
        }
      } catch { /* skip */ }
    }
    output.appendLine(
      `Built sourceFileMap with ${Object.keys(sourceFileMap).length} entries.`,
    );

    const setupCommands: { description: string; text: string; ignoreFailures: boolean }[] = [
      {
        description: 'Pascal case-insensitive name lookup',
        text: '-gdb-set case-sensitive off',
        ignoreFailures: true,
      },
      {
        // Make gdb report absolute paths in stop / frame events.
        // Default ("relative") returns just the DW_AT_name from the
        // DWARF, which is a bare filename when comp_dir is empty.
        // cppdbg then has no path to give VSCode, and the editor
        // silently fails to open the source.
        description: 'Report absolute source paths',
        text: '-gdb-set filename-display absolute',
        ignoreFailures: true,
      },
    ];
    for (const dir of searchPaths) {
      const gdbPathStyle = dir.replace(/\\/g, '/');
      // Use the MI form -environment-directory. It accepts a single
      // C-string argument, which preserves spaces (unlike gdb's
      // console-syntax `directory` command that splits on whitespace
      // and breaks paths like "C:/Source code").
      setupCommands.push({
        description: `Add source dir: ${dir}`,
        text: `-environment-directory "${gdbPathStyle}"`,
        ignoreFailures: true,
      });
    }
    // Final probe so we can see in the Debug Console what gdb actually
    // has after our setupCommands run.
    setupCommands.push({
      description: 'show source dirs (probe)',
      text: '-interpreter-exec console "show directories"',
      ignoreFailures: true,
    });

    const backend = vscode.workspace.getConfiguration('rsm2pdb')
      .get<string>('backend') ?? 'pdb';

    // cppvsdbg (VS native engine) reads PDBs directly. No gdb / MI /
    // setupCommands needed; sourceFileMap (basename -> abs path) still
    // works the same way for resolving the bare-filename paths Delphi
    // stores in the .map.
    // Route the debuggee's stdout / stderr through VSCode's integrated
    // Terminal panel rather than spawning a fresh Windows console
    // window. The bare-console window that used to pop up on every
    // launch was visually distracting and stole focus from VSCode --
    // and for Delphi GUI apps it was useless anyway. Users who want
    // the old behaviour can flip rsm2pdb.console -> "external".
    const consolePref =
      vscode.workspace.getConfiguration('rsm2pdb')
        .get<string>('console') ?? 'integrated';
    const useExternalConsole = consolePref === 'external';
    const launch: vscode.DebugConfiguration = backend === 'pdb'
      ? {
          name: `rsm2pdb: ${settings.projectName} (${chosen.name} ${chosen.platform})`,
          type: 'cppvsdbg',
          request: 'launch',
          program,
          cwd: settings.debuggerCwd ?? path.dirname(program),
          args: settings.debuggerRunParams ? [settings.debuggerRunParams] : [],
          environment: [],
          // cppvsdbg console values: 'internalConsole' | 'integratedTerminal' |
          // 'externalTerminal' | 'newExternalWindow'. integratedTerminal
          // keeps Writeln output in the VSCode terminal panel and
          // doesn't open a separate Windows console.
          console: useExternalConsole ? 'externalTerminal' : 'integratedTerminal',
          stopAtEntry: false,
          sourceFileMap,
        }
      : {
          name: `rsm2pdb: ${settings.projectName} (${chosen.name} ${chosen.platform})`,
          type: 'cppdbg',
          request: 'launch',
          program,
          cwd: settings.debuggerCwd ?? path.dirname(program),
          args: settings.debuggerRunParams ? [settings.debuggerRunParams] : [],
          environment: [],
          // cppdbg: externalConsole=false routes stdout/stderr through
          // the integrated Debug Console. There's no integratedTerminal
          // analogue here -- the C/C++ extension only exposes the
          // boolean -- so Debug Console it is.
          externalConsole: useExternalConsole,
          MIMode: 'gdb',
          miDebuggerPath: gdbPath,
          stopAtEntry: false,
          sourceFileMap,
          setupCommands,
        };

    output.appendLine(`\nLaunching ${launch.type} with program: ${program}`);
    output.appendLine(`  cwd:  ${launch.cwd}`);
    output.appendLine(`  args: ${JSON.stringify(launch.args)}`);
    const ws = vscode.workspace.workspaceFolders?.[0];
    const started = await vscode.debug.startDebugging(ws, launch);
    if (!started) {
      output.appendLine(
        `cppdbg returned false. Likely the C/C++ extension (ms-vscode.cpptools) ` +
          `is not installed or failed to initialize.`,
      );
      vscode.window.showErrorMessage(
        `rsm2pdb: failed to start debugger (cppdbg). Is the C/C++ extension installed?`,
      );
    } else {
      output.appendLine(`Debug session started.`);
    }
  };

  // -------- rsm2pdb.buildAndDebug --------
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.buildAndDebug', async () => {
      output.show(true);
      const pick = await getActivePick(false);
      if (!pick) return;
      const project = pick.project;
      const chosen = pick.config;
      const settings = resolveSettings(
        project.dprojPath, chosen.cfgId, chosen.platform,
      );

      const result = await runBuild(output, {
        dprojPath: project.dprojPath,
        configName: chosen.name,
        platform: chosen.platform,
        settings,
        target: 'Make',
        extensionPath: context.extensionPath,
        diagnostics,
      });
      if (!result.ok) return;

      // Build a cppdbg launch config from the resolved settings + user prefs.
      const gdbPath =
        vscode.workspace.getConfiguration('rsm2pdb').get<string>('gdbPath') ?? '';
      const program = settings.appType === 'Library'
        ? (settings.hostApplication ?? '')
        : result.outputPath;
      if (!program) {
        output.appendLine(
          `error: cannot determine debug program path. For Library projects, ` +
            `the .dproj must set <Debugger_HostApplication>.`,
        );
        return;
      }

      await startDebugSession(settings, chosen);
    }),
  );

  // -------- rsm2pdb.debugLast --------
  // Start a debug session against the LAST built artifact, with NO
  // build / rsm2pdb step. Uses the persisted (project, config, platform)
  // from a prior build/debug invocation. Falls back to a picker if no
  // last-pick is on record.
  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.debugLast', async () => {
      output.show(true);
      const last = context.workspaceState.get<LastPick>(kLastPickKey);
      if (!last) {
        output.appendLine(
          `No previous build in this workspace; pick a project + config.`,
        );
        const project = await pickProject(output);
        if (!project) return;
        const chosen = await pickConfig(project.dprojPath);
        if (!chosen) return;
        await rememberPick(project, chosen);
        const settings = resolveSettings(
          project.dprojPath, chosen.cfgId, chosen.platform,
        );
        await startDebugSession(settings, chosen);
        return;
      }
      output.appendLine(
        `\n=== Debug (no build): ${path.basename(last.dprojPath)} -- ` +
          `${last.cfgName} [${last.platform}] ===`,
      );
      const settings = resolveSettings(
        last.dprojPath, last.cfgId, last.platform,
      );
      await startDebugSession(
        settings,
        {
          cfgId: last.cfgId,
          name: last.cfgName,
          platform: last.platform,
        },
      );
    }),
  );

  // -------- Status bar buttons --------
  //
  // Pin a small group of one-click actions to the bottom-left of the
  // status bar so the user doesn't have to remember the keybindings or
  // open the Command Palette. Right-aligned to keep the very-left
  // (errors / warnings) area clean. Each item just invokes its
  // already-registered command -- the buttons are pure UI.
  //
  // Layout: [ Build ] [ Rebuild ] [ Run ] [ Rerun ] [ Cancel ]
  // Priority decreases left-to-right so the order is stable.
  const makeStatusItem = (
    text: string,
    tooltip: string,
    command: string,
    priority: number,
  ): vscode.StatusBarItem => {
    const item = vscode.window.createStatusBarItem(
      vscode.StatusBarAlignment.Left,
      priority,
    );
    item.text    = text;
    item.tooltip = tooltip;
    item.command = command;
    item.show();
    context.subscriptions.push(item);
    return item;
  };
  // Priorities chosen as a small descending block so the order is
  // Build -> Rebuild -> Run -> Rerun -> Cancel from left to right
  // (status bar items with higher priority render more to the left).
  makeStatusItem(
    '$(tools) Build',
    'rsm2pdb: incremental build (Ctrl+B)',
    'rsm2pdb.build',
    1005,
  );
  makeStatusItem(
    '$(refresh) Rebuild',
    'rsm2pdb: full clean rebuild (Ctrl+Shift+B)',
    'rsm2pdb.rebuild',
    1004,
  );
  makeStatusItem(
    '$(debug-start) Run',
    'rsm2pdb: launch last build (no rebuild) (Ctrl+F9)',
    'rsm2pdb.debugLast',
    1003,
  );
  makeStatusItem(
    '$(debug-restart) Make+Run',
    'rsm2pdb: incremental Make (no clean) + launch (Ctrl+Shift+F9)',
    'rsm2pdb.buildAndDebug',
    1002,
  );
  const cancelItem = makeStatusItem(
    '$(stop-circle) Cancel',
    'rsm2pdb: cancel current build (Ctrl+F2)',
    'rsm2pdb.cancelBuild',
    1001,
  );

  // -------- Active-pick status bar items --------
  //
  // Two dropdowns sit to the LEFT of the Build/Rebuild/Run/Make+Run
  // cluster (higher priority = more to the left in the status bar):
  //
  //   [$(package) <projectName>] [$(gear) <cfgName> <platform>]
  //
  // Clicking the project pill opens a project picker; clicking the
  // config pill opens a config picker scoped to the currently-active
  // project. Build / Rebuild / Make+Run use whatever is currently
  // selected, never re-prompting.
  const projectPickItem = vscode.window.createStatusBarItem(
    vscode.StatusBarAlignment.Left,
    1007,
  );
  projectPickItem.command = 'rsm2pdb.pickActiveProject';
  projectPickItem.show();
  context.subscriptions.push(projectPickItem);

  const configPickItem = vscode.window.createStatusBarItem(
    vscode.StatusBarAlignment.Left,
    1006,
  );
  configPickItem.command = 'rsm2pdb.pickActiveConfig';
  configPickItem.show();
  context.subscriptions.push(configPickItem);

  refreshActivePickStatusBar = (): void => {
    const last = context.workspaceState.get<LastPick>(kLastPickKey);
    if (!last) {
      projectPickItem.text = '$(package) rsm2pdb: pick project...';
      projectPickItem.tooltip =
        'No active Delphi project chosen. Click to pick a .dproj.';
      configPickItem.text = '$(gear) (no config)';
      configPickItem.tooltip =
        'Pick a project first; the configuration picker reads its ' +
        '(Config, Platform) list from the active .dproj.';
      return;
    }
    const proj = path.basename(last.dprojPath, '.dproj');
    projectPickItem.text = `$(package) ${proj}`;
    projectPickItem.tooltip =
      `rsm2pdb active project:\n` +
      `  .dproj : ${last.dprojPath}\n\n` +
      `Click to switch to another .dproj.`;
    configPickItem.text =
      `$(gear) ${last.cfgName} ${last.platform}`;
    configPickItem.tooltip =
      `rsm2pdb active configuration:\n` +
      `  config   : ${last.cfgName} (${last.cfgId})\n` +
      `  platform : ${last.platform}\n\n` +
      `Click to switch (lists configs from the active .dproj).`;
  };
  refreshActivePickStatusBar();
  // Highlight the Cancel button with the warning background while a
  // build is in progress so the user can spot it instantly. Without
  // this the button looks identical to the others -- easy to miss
  // when a build is stuck and you're scanning the status bar in a
  // hurry.
  const refreshCancelHighlight = (running: boolean): void => {
    cancelItem.backgroundColor = running
      ? new vscode.ThemeColor('statusBarItem.warningBackground')
      : undefined;
    cancelItem.tooltip = running
      ? 'rsm2pdb: BUILD RUNNING -- click to kill the build (Ctrl+F2)'
      : 'rsm2pdb: cancel current build (Ctrl+F2)';
  };
  refreshCancelHighlight(isBuildRunning());
  context.subscriptions.push(onBuildStateChange(refreshCancelHighlight));

  // -------- Skip-stops-in-source-less code on step --------
  //
  // When the user steps into a function whose source file we don't
  // have (typically Delphi RTL or external libraries), the debugger
  // stops in disassembly / a blank editor and the user loses context.
  // We intercept the `stopped` event from the debug adapter and -- if
  // the active frame has no source file we can open -- automatically
  // step out until execution lands in user code.
  //
  // Modes (rsm2pdb.skipUnknownSourceMode):
  //   "off"     -- never auto-skip; stop wherever the debugger says.
  //   "out"     -- default. On a step that lands in source-less code,
  //                issue stepOut up to `kStepOutBudget` times. Fast,
  //                always-safe: we only ever climb the stack, so we
  //                can't accidentally execute past a user breakpoint.
  //   "hybrid"  -- first try `kStepInBudget` stepIns to descend into
  //                a possible deeper user-code callback (event
  //                handler / virtual override / lambda body), then
  //                fall back to stepOut. Catches more cases but the
  //                stepIn chain can blow past user breakpoints that
  //                live in code the chain crosses, since adapters
  //                may report stops from BPs hit mid-step as
  //                reason='step'. Opt-in for users who routinely
  //                debug callbacks invoked from RTL.
  //
  // Only triggers after step events (reason='step') AND only when
  // the stop did NOT carry hitBreakpointIds -- if the adapter
  // reports a BP was hit (even alongside reason='step'), we stop
  // auto-stepping immediately so the user lands on their BP.
  interface AutoStepState {
    sessionId: string;
    threadId: number;
    inBudget:  number;
    outBudget: number;
  }
  let autoStepState: AutoStepState | undefined;
  const kStepInBudget  = 50;
  const kStepOutBudget = 30;

  const frameHasReadableSource = async (
    session: vscode.DebugSession,
    threadId: number,
  ): Promise<boolean> => {
    try {
      const r = await session.customRequest('stackTrace', {
        threadId,
        startFrame: 0,
        levels: 1,
      });
      const frame = r?.stackFrames?.[0];
      const p: string | undefined = frame?.source?.path;
      if (!p) return false;
      try {
        return fs.existsSync(p);
      } catch {
        return false;
      }
    } catch {
      return false;
    }
  };

  const trackerFactory: vscode.DebugAdapterTrackerFactory = {
    createDebugAdapterTracker(session: vscode.DebugSession) {
      return {
        onDidSendMessage: (msg: any) => {
          const cfg = vscode.workspace.getConfiguration('rsm2pdb');
          // Backward compat: the older boolean form still works as
          // on/off mapped to "out"/"off" respectively.
          let mode = cfg.get<string>('skipUnknownSourceMode');
          if (mode === undefined) {
            const legacy = cfg.get<boolean>('skipUnknownSource');
            mode = legacy === false ? 'off' : 'out';
          }
          if (mode === 'off') return;
          if (!msg || msg.type !== 'event' || msg.event !== 'stopped') return;
          const reason: string | undefined = msg.body?.reason;
          const threadId: number | undefined = msg.body?.threadId;
          const hitBps: unknown = msg.body?.hitBreakpointIds;
          if (typeof threadId !== 'number') return;

          // Any hit BP -- regardless of how the adapter classified the
          // stop reason -- cancels auto-stepping. Without this safety
          // check the stepIn chain in hybrid mode would race past
          // user-code breakpoints that fall inside the stepped-through
          // region (some adapters report reason='step' even when
          // hitBreakpointIds is populated).
          const stoppedOnBp =
            (Array.isArray(hitBps) && hitBps.length > 0) ||
            reason === 'breakpoint' ||
            reason === 'function breakpoint' ||
            reason === 'data breakpoint' ||
            reason === 'instruction breakpoint';
          if (stoppedOnBp) {
            if (autoStepState && autoStepState.sessionId === session.id) {
              autoStepState = undefined;
            }
            return;
          }

          // Reset state on other non-step stops (pause / entry /
          // exception). Those are intentional landings.
          if (reason !== 'step') {
            if (autoStepState && autoStepState.sessionId === session.id) {
              autoStepState = undefined;
            }
            return;
          }

          // Defer the source check + next step to the next tick so
          // VSCode finishes its own bookkeeping around the stop event
          // before we issue a new request.
          setImmediate(async () => {
            if (!autoStepState
                || autoStepState.sessionId !== session.id
                || autoStepState.threadId !== threadId) {
              autoStepState = {
                sessionId: session.id,
                threadId,
                // Only the "hybrid" mode descends; "out" mode gets a
                // zero descend budget so we always climb the stack.
                inBudget:  mode === 'hybrid' ? kStepInBudget : 0,
                outBudget: kStepOutBudget,
              };
            }
            if (await frameHasReadableSource(session, threadId)) {
              // Landed in source we can show -- stop here.
              autoStepState = undefined;
              return;
            }
            // Phase 1 (hybrid only): try to descend further with
            // stepIn. If the RTL routine eventually calls back into
            // user code, we'll land in the deeper user-code frame.
            if (autoStepState.inBudget > 0) {
              autoStepState.inBudget--;
              try {
                await session.customRequest('stepIn', { threadId });
              } catch {
                autoStepState = undefined;
              }
              return;
            }
            // Phase 2: climb the stack. stepOut never executes user
            // code (it sets a temp BP at the return address and
            // resumes), so it can't accidentally bypass a user
            // breakpoint that lives further down the same function.
            if (autoStepState.outBudget > 0) {
              autoStepState.outBudget--;
              try {
                await session.customRequest('stepOut', { threadId });
              } catch {
                autoStepState = undefined;
              }
              return;
            }
            // Budgets exhausted -- give up; user sees where they are
            // (probably deep RTL) and can decide manually.
            autoStepState = undefined;
          });
        },
        onWillStopSession: () => {
          if (autoStepState && autoStepState.sessionId === session.id) {
            autoStepState = undefined;
          }
        },
      };
    },
  };
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterTrackerFactory('cppvsdbg', trackerFactory),
  );
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterTrackerFactory('cppdbg', trackerFactory),
  );

  // -------- "View as ..." context menu in Variables / Watch -------
  //
  // Lets the user re-interpret a variable's storage on the fly. Useful
  // when a value comes through as void* / native int because the type
  // info was incomplete -- the user can right-click in the Variables
  // or Watch panel and say "show it as a wchar_t* string", "as an
  // AnsiString", "as a 16-byte hex dump", etc. The debugger evaluates
  // the cast expression and we surface the result in:
  //   1. A toast (so it's visible immediately), and
  //   2. The Debug Console (so the user has a scrollable history).
  //
  // The context-menu argument shape varies a little between VSCode
  // builds, but the relevant fields are always reachable as either
  // `arg.variable.evaluateName` (most reliable) or `arg.variable.name`
  // / `arg.name`. We probe defensively.
  type DebugVariableArg = {
    variable?: {
      name?: string;
      evaluateName?: string;
      value?: string;
      type?: string;
      variablesReference?: number;
    };
    container?: { variablesReference?: number };
    sessionId?: string;
    name?: string;
    evaluateName?: string;
  };
  const getExpr = (arg: DebugVariableArg | undefined): string | undefined => {
    if (!arg) return undefined;
    return arg.variable?.evaluateName
        ?? arg.variable?.name
        ?? arg.evaluateName
        ?? arg.name;
  };
  const evalAndShow = async (
    castExpr: string,
    label: string,
  ): Promise<void> => {
    const session = vscode.debug.activeDebugSession;
    if (!session) {
      vscode.window.showWarningMessage(
        'rsm2pdb: no active debug session.',
      );
      return;
    }
    const frameId = (vscode.debug.activeStackItem as any)?.frameId;
    try {
      // 'watch' context returns the prettified watch-style result; we
      // mirror it to 'repl' so the Debug Console grows a history.
      const r: any = await session.customRequest('evaluate', {
        expression: castExpr,
        frameId,
        context: 'watch',
      });
      const result = r?.result ?? '<no result>';
      vscode.window.showInformationMessage(
        `${label}:  ${castExpr}  =  ${result}`,
      );
      await session.customRequest('evaluate', {
        expression: castExpr,
        frameId,
        context: 'repl',
      });
    } catch (e: any) {
      vscode.window.showErrorMessage(
        `rsm2pdb: evaluate failed for ${castExpr}: ` +
          `${e?.message ?? String(e)}`,
      );
    }
  };
  const registerViewAs = (
    cmd: string,
    label: string,
    buildExpr: (n: string) => string,
  ): void => {
    context.subscriptions.push(
      vscode.commands.registerCommand(cmd, async (arg: DebugVariableArg) => {
        const name = getExpr(arg);
        if (!name) {
          vscode.window.showWarningMessage(
            'rsm2pdb: no variable selected.',
          );
          return;
        }
        await evalAndShow(buildExpr(name), label);
      }),
    );
  };
  // Pointer-to-char family -- relies on cppvsdbg auto-display for
  // null-terminated strings at the pointer target. For Pascal RTL
  // managed strings (UnicodeString / AnsiString) the data buffer
  // lives at the pointer's exact address, so a bare cast suffices.
  registerViewAs(
    'rsm2pdb.viewAsString',
    'as UnicodeString',
    (n) => `(wchar_t*)(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsAnsiString',
    'as AnsiString',
    (n) => `(char*)(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsWideString',
    'as WideString',
    (n) => `(wchar_t*)(${n})`,
  );
  // Integer reinterpretations (works on pointer-sized / int-sized
  // values). The `*(T*)&expr` form re-interprets the storage bytes
  // without changing the address.
  registerViewAs(
    'rsm2pdb.viewAsByte',
    'as Byte',
    (n) => `*(unsigned char*)&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsWord',
    'as Word',
    (n) => `*(unsigned short*)&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsInteger',
    'as Integer',
    (n) => `*(int*)&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsCardinal',
    'as Cardinal',
    (n) => `*(unsigned int*)&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsInt64',
    'as Int64',
    (n) => `*(__int64*)&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsUInt64',
    'as UInt64',
    (n) => `*(unsigned __int64*)&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsSingle',
    'as Single',
    (n) => `*(float*)&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsDouble',
    'as Double',
    (n) => `*(double*)&(${n})`,
  );
  // Byte-array dump: a pointer or address-of expression cast to a
  // fixed-size unsigned-char array. The user can extend by editing
  // the watch entry that lands in Debug Console; default 16 bytes.
  registerViewAs(
    'rsm2pdb.viewAsByteArray16',
    'as Byte[16]',
    (n) => `*(unsigned char(*)[16])&(${n})`,
  );
  registerViewAs(
    'rsm2pdb.viewAsByteArray64',
    'as Byte[64]',
    (n) => `*(unsigned char(*)[64])&(${n})`,
  );
}

export function deactivate(): void {
  // nothing to clean up
}
