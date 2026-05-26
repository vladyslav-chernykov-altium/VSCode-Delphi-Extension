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

import {
  listConfigs,
  resolveSettings,
  type DprojConfig,
  type Platform,
} from './dprojParser';
import { runBuild, cancelBuild } from './buildRunner';

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
  };

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
      const project = await pickProject(output);
      if (!project) return;
      const chosen = await pickConfig(project.dprojPath);
      if (!chosen) return;
      await rememberPick(project, chosen);
      const settings = resolveSettings(
        project.dprojPath, chosen.cfgId, chosen.platform,
      );
      await runBuild(output, {
        dprojPath: project.dprojPath,
        configName: chosen.name,
        platform: chosen.platform,
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
      const project = await pickProject(output);
      if (!project) return;
      const chosen = await pickConfig(project.dprojPath);
      if (!chosen) return;
      await rememberPick(project, chosen);
      const settings = resolveSettings(
        project.dprojPath, chosen.cfgId, chosen.platform,
      );
      await runBuild(output, {
        dprojPath: project.dprojPath,
        configName: chosen.name,
        platform: chosen.platform,
        settings,
        target: 'Rebuild',
        extensionPath: context.extensionPath,
        diagnostics,
      });
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

    const launch: vscode.DebugConfiguration = {
      name: `rsm2pdb: ${settings.projectName} (${chosen.name} ${chosen.platform})`,
      type: 'cppdbg',
      request: 'launch',
      program,
      cwd: settings.debuggerCwd ?? path.dirname(program),
      args: settings.debuggerRunParams ? [settings.debuggerRunParams] : [],
      environment: [],
      externalConsole: true,
      MIMode: 'gdb',
      miDebuggerPath: gdbPath,
      stopAtEntry: false,
      sourceFileMap,
      setupCommands,
    };

    output.appendLine(`\nLaunching cppdbg with program: ${program}`);
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
      const project = await pickProject(output);
      if (!project) return;
      const chosen = await pickConfig(project.dprojPath);
      if (!chosen) return;
      await rememberPick(project, chosen);
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

}

export function deactivate(): void {
  // nothing to clean up
}
