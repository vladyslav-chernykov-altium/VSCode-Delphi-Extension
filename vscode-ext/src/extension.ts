// rsm2pdb-vscode  —  Delphi build + DWARF inject + debug launch.
//
// Skeleton. Commands are stubbed; the real work is delegated to
// scripts/delphi-debug.ps1 in the parent repo. The extension's job
// will be:
//
//   1. Discover .dproj files in the workspace.
//   2. Let the user pick one and a config (Debug/Release).
//   3. Invoke scripts/delphi-debug.ps1 (or call rsm2pdb.exe directly
//      once we port the wrapper to TypeScript).
//   4. Write/update a launch.json entry that targets the produced
//      DWARF-enriched .exe.
//   5. Trigger vscode.debug.startDebugging().
//
// Nothing here is wired up yet beyond placeholders. Build with
//   cd vscode-ext && npm install && npm run compile
// to produce dist/extension.js. Then F5 in vscode-ext/ launches an
// Extension Development Host. See README.md.

import * as vscode from 'vscode';
import * as path from 'path';

interface DelphiProject {
  dprojPath: string;
  projectName: string;
}

/**
 * Scan the active workspace for .dproj files. Skeleton: returns the
 * first match. Real impl will return all + let user pick.
 */
async function findDelphiProjects(): Promise<DelphiProject[]> {
  const found = await vscode.workspace.findFiles(
    '**/*.dproj',
    '**/node_modules/**',
    50
  );
  return found.map((u) => ({
    dprojPath: u.fsPath,
    projectName: path.basename(u.fsPath, '.dproj'),
  }));
}

/**
 * Run scripts/delphi-debug.ps1 as a child process and stream output
 * to a dedicated OutputChannel. Skeleton: just logs the command line
 * it would run.
 */
async function runBuild(
  output: vscode.OutputChannel,
  dprojPath: string,
  config: 'Debug' | 'Release'
): Promise<{ ok: boolean; exePath: string }> {
  output.appendLine(`[skeleton] would build: ${dprojPath} (${config})`);
  output.appendLine(`[skeleton] equivalent of:`);
  output.appendLine(
    `  powershell -NoProfile -ExecutionPolicy Bypass ` +
      `-File <repo>/scripts/delphi-debug.ps1 ` +
      `-Dproj "${dprojPath}" -Config ${config}`
  );
  const dir = path.dirname(dprojPath);
  const projName = path.basename(dprojPath, '.dproj');
  const exePath = path.join(dir, 'Win64', config, `${projName}.exe`);
  return { ok: false, exePath };
}

/**
 * Generate or update a cppdbg launch configuration that points at
 * the just-built (DWARF-enriched) .exe. Skeleton: dumps the JSON we
 * would write.
 */
async function writeLaunchEntry(
  output: vscode.OutputChannel,
  exePath: string,
  sourceRoot: string
): Promise<void> {
  const gdbPath =
    vscode.workspace
      .getConfiguration('rsm2pdb')
      .get<string>('gdbPath') ?? '';
  const entry = {
    name: `Delphi: ${path.basename(exePath, '.exe')}`,
    type: 'cppdbg',
    request: 'launch',
    program: exePath,
    cwd: path.dirname(exePath),
    MIMode: 'gdb',
    miDebuggerPath: gdbPath,
    externalConsole: true,
    stopAtEntry: false,
    sourceFileMap: {
      // populated per project once real impl scans the .dproj's
      // DCCReference items and pairs filename -> absolute path
    },
  };
  output.appendLine('[skeleton] would write launch entry:');
  output.appendLine(JSON.stringify(entry, null, 2));
  output.appendLine('[skeleton] target source root: ' + sourceRoot);
}

export function activate(context: vscode.ExtensionContext): void {
  const output = vscode.window.createOutputChannel('rsm2pdb');

  context.subscriptions.push(output);

  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.buildAndDebug', async () => {
      output.show(true);
      output.appendLine('rsm2pdb: buildAndDebug invoked');

      const projects = await findDelphiProjects();
      if (projects.length === 0) {
        vscode.window.showErrorMessage('rsm2pdb: no .dproj found in workspace');
        return;
      }
      const pick =
        projects.length === 1
          ? projects[0]
          : await vscode.window.showQuickPick(
              projects.map((p) => ({ label: p.projectName, project: p })),
              { placeHolder: 'Pick a Delphi project' }
            ).then((c) => c?.project);
      if (!pick) return;

      const config = (await vscode.window.showQuickPick(['Debug', 'Release'], {
        placeHolder: 'Build configuration',
      })) as 'Debug' | 'Release' | undefined;
      if (!config) return;

      const result = await runBuild(output, pick.dprojPath, config);
      await writeLaunchEntry(output, result.exePath, path.dirname(pick.dprojPath));

      output.appendLine('[skeleton] vscode.debug.startDebugging() not wired yet');
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.build', async () => {
      output.show(true);
      output.appendLine('rsm2pdb: build invoked (skeleton; no-op)');
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('rsm2pdb.pickProject', async () => {
      const projects = await findDelphiProjects();
      output.show(true);
      output.appendLine(`rsm2pdb: found ${projects.length} .dproj file(s)`);
      for (const p of projects) {
        output.appendLine(`  ${p.projectName}  (${p.dprojPath})`);
      }
    })
  );
}

export function deactivate(): void {
  // nothing to clean up
}
