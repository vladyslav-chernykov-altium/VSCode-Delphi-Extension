// Build runner: drives rsvars + msbuild + rsm2pdb dwarf for a chosen
// Delphi project + (Config, Platform). Streams output to a VSCode
// OutputChannel so the user sees progress live.

import * as cp from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';

import type { DprojSettings, Platform } from './dprojParser';

export interface BuildResult {
  ok: boolean;
  error?: string;
  /** Resolved path to the DWARF-enriched DLL/EXE on success. */
  outputPath: string;
}

// ---------------- Delphi diagnostic parser ----------------
//
// dcc64's compact-output format (as emitted by msbuild's
// _PasCoreCompile target) varies a bit between versions:
//
//   <file>.pas(<line>) Error: E2010 <message>
//   <file>.pas(<line>) Warning: W1011 <message>
//   <file>.pas(<line>,<col>) Error: E2010 <message>
//   <file>.pas(<line>): Hint H2369 <message>
//   <file>.pas(<line>): Hint warning H2443 <message>    <-- two-word
//   <file>.pas(<line>): Warning W1011 <message>
//   [dcc64 Error] <file>.pas(<line>): E2010 <message>
//
// The severity tag is captured as a phrase ("Hint warning" / "Fatal
// Error" / etc.), then we also accept an optional W####/H####/E####
// code anywhere in the leading region of the message.
const kDelphiDiagnostic = new RegExp(
  String.raw`^\s*(?:\[dcc(?:32|64)?\s+[^\]]+\]\s*)?` +
    String.raw`(.+?\.(?:pas|dpr|inc))` +     // 1: file
    String.raw`\((\d+)(?:,\s*(\d+))?\)` +    // 2: line, 3: col?
    String.raw`\s*:?\s*` +
    // 4: severity tag -- "Hint", "Hint warning", "Warning",
    //    "Error", or "Fatal Error" (case-insensitive).
    String.raw`(Hint\s+warning|Hint|Warning|Fatal\s*Error|Error)` +
    String.raw`\s*:?\s*` +
    String.raw`(?:([EWHF]\d{3,5})\s*:?\s*)?` + // 5: code?
    String.raw`(.*)$`,                          // 6: message
  'i',
);

interface ParsedDiagnostic {
  file: string;
  line: number;            // 1-based as Delphi emits
  col: number;             // 1-based; defaults to 1
  severity: vscode.DiagnosticSeverity;
  code: string;
  message: string;
}

function parseDiagnostic(line: string): ParsedDiagnostic | undefined {
  const m = kDelphiDiagnostic.exec(line);
  if (!m) return undefined;
  const sevText = m[4].toLowerCase().replace(/\s+/g, ' ').trim();
  let severity: vscode.DiagnosticSeverity;
  // Map Delphi severities to vscode.DiagnosticSeverity. Note that
  // Delphi's "Hint" -- which is informational compiler advice -- is
  // mapped to vscode.Information (not vscode.Hint) because the
  // latter does not show as a standalone row in the PROBLEMS panel.
  // gdb's compiler "hints" are still useful to surface; users can
  // filter them out via the Problems panel filter if they're noisy.
  if (sevText === 'error' || sevText.startsWith('fatal')) {
    severity = vscode.DiagnosticSeverity.Error;
  } else if (sevText === 'warning') {
    severity = vscode.DiagnosticSeverity.Warning;
  } else if (sevText.startsWith('hint')) {
    severity = vscode.DiagnosticSeverity.Information;
  } else {
    severity = vscode.DiagnosticSeverity.Information;
  }
  return {
    file: m[1].trim(),
    line: parseInt(m[2], 10),
    col: m[3] ? parseInt(m[3], 10) : 1,
    severity,
    code: m[5] ?? '',
    message: (m[6] ?? '').trim(),
  };
}

// -- module-level state for cancellation -------------------------------
let currentChild: cp.ChildProcess | undefined;
let currentChildLabel: string = '';
let cancelled = false;

/**
 * Cancel the running build, if any. Kills the spawned process AND its
 * entire process tree (msbuild spawns dcc64.exe; killing just cmd.exe
 * leaves dcc64 running). Safe to call when no build is in progress.
 */
export function cancelBuild(output: vscode.OutputChannel): void {
  if (!currentChild || currentChild.pid === undefined) {
    output.appendLine('\n[cancel: nothing to cancel]');
    return;
  }
  cancelled = true;
  const pid = currentChild.pid;
  output.appendLine(`\n[cancelling: ${currentChildLabel} pid=${pid}]`);
  try {
    cp.execSync(`taskkill /pid ${pid} /T /F`, { stdio: 'ignore' });
  } catch {
    // Best-effort. If taskkill fails (already exited, etc.) ignore.
  }
}

interface BuildPlan {
  dprojPath: string;
  configName: string;          // e.g. "Debug :: Built-In"
  platform: Platform;
  settings: DprojSettings;
  /** MSBuild target (Delphi conventions):
   *    "Make"    = incremental, compile only changed units
   *    "Build"   = full build (compile all units, no clean)
   *    "Rebuild" = clean + build  */
  target: 'Make' | 'Build' | 'Rebuild';
  /** Filesystem path where this extension is installed. Used to locate
   *  rsm2pdb.exe relative to the sibling build/ tree when the user
   *  hasn't set `rsm2pdb.executable` explicitly. */
  extensionPath: string;
  /** DiagnosticCollection to populate with parsed compiler hints /
   *  warnings / errors. Cleared at the start of each build. */
  diagnostics?: vscode.DiagnosticCollection;
}

interface ResolvedTools {
  bdsPath: string;             // Embarcadero Studio install root
  rsvarsBat: string;
  rsm2pdbExe: string;
}

/**
 * Run the full Delphi -> DWARF pipeline:
 *   1. rsvars.bat                (Delphi env)
 *   2. msbuild <dproj>           (produces .exe/.dll + .map + .rsm)
 *   3. rsm2pdb dwarf             (in-place DWARF injection)
 */
export async function runBuild(
  output: vscode.OutputChannel,
  plan: BuildPlan,
): Promise<BuildResult> {
  output.show(true);
  cancelled = false;
  const banner = `${plan.configName} [${plan.platform}]`;
  const t0 = Date.now();
  const stamp = () => {
    const s = ((Date.now() - t0) / 1000).toFixed(2).padStart(6);
    return `[${s}s]`;
  };
  output.appendLine(`\n=== Build: ${path.basename(plan.dprojPath)} -- ${banner} ===`);
  output.appendLine(`${stamp()} target=${plan.target}`);

  // Reset diagnostics for this build cycle.
  if (plan.diagnostics) plan.diagnostics.clear();
  const diagAccumulator = new Map<string, vscode.Diagnostic[]>();
  const dprojDir = path.dirname(plan.dprojPath);

  output.appendLine(`${stamp()} Resolving tools...`);
  const tools = resolveTools(output, plan.extensionPath);
  if (!tools) {
    return {
      ok: false,
      error: 'tool paths not resolvable; see Output for details',
      outputPath: '',
    };
  }

  // ----- 1+2. msbuild via rsvars -----
  output.appendLine(`${stamp()} Starting msbuild (/t:${plan.target})...`);
  const tBuild0 = Date.now();
  const msbuildOk = await runMsbuild(
    output, plan, tools, diagAccumulator, dprojDir,
  );
  output.appendLine(
    `${stamp()} msbuild finished in ${((Date.now() - tBuild0) / 1000).toFixed(2)}s ` +
      `(${msbuildOk ? 'ok' : 'FAIL'})`,
  );
  // Publish whatever diagnostics we saw, even on failure.
  publishDiagnostics(plan.diagnostics, diagAccumulator);
  if (cancelled) {
    return { ok: false, error: 'cancelled', outputPath: '' };
  }
  if (!msbuildOk) {
    return { ok: false, error: 'msbuild failed', outputPath: '' };
  }

  // ----- 3. rsm2pdb dwarf -----
  output.appendLine(`${stamp()} Injecting DWARF via rsm2pdb...`);
  const tDwarf0 = Date.now();
  const dwarfOk = await runRsm2pdb(output, plan, tools);
  output.appendLine(
    `${stamp()} rsm2pdb finished in ${((Date.now() - tDwarf0) / 1000).toFixed(2)}s ` +
      `(${dwarfOk ? 'ok' : 'FAIL'})`,
  );
  if (cancelled) {
    return { ok: false, error: 'cancelled', outputPath: '' };
  }
  if (!dwarfOk) {
    return { ok: false, error: 'rsm2pdb dwarf failed', outputPath: '' };
  }

  output.appendLine(
    `${stamp()} Build succeeded -> ${plan.settings.outputFullPath}`,
  );
  return { ok: true, outputPath: plan.settings.outputFullPath };
}

// ---------------- helpers ----------------

function resolveTools(
  output: vscode.OutputChannel,
  extensionPath: string,
): ResolvedTools | undefined {
  const cfg = vscode.workspace.getConfiguration('rsm2pdb');
  const bdsPath = cfg.get<string>('bdsPath') ?? '';
  let rsm2pdbExe = cfg.get<string>('executable') ?? '';

  // Substitute ${workspaceFolder} in rsm2pdb executable path.
  const ws = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (ws) rsm2pdbExe = rsm2pdbExe.replace(/\$\{workspaceFolder\}/g, ws);

  // Fallback: if the configured (or default-substituted) path doesn't
  // exist, try the sibling build/ directory of the extension itself.
  // In development the layout is:
  //    <repo>/build/src/rsm2pdb.exe
  //    <repo>/vscode-ext/         <-- extensionPath
  // So extensionPath/../build/src/rsm2pdb.exe is the answer.
  if (!rsm2pdbExe || !fs.existsSync(rsm2pdbExe)) {
    const fallback = path.resolve(
      extensionPath, '..', 'build', 'src', 'rsm2pdb.exe',
    );
    if (fs.existsSync(fallback)) {
      output.appendLine(
        `rsm2pdb.executable not set or path missing; using fallback ` +
          `relative to extension: ${fallback}`,
      );
      rsm2pdbExe = fallback;
    }
  }

  const rsvarsBat = path.join(bdsPath, 'bin', 'rsvars.bat');

  let ok = true;
  if (!bdsPath) {
    output.appendLine(`error: rsm2pdb.bdsPath setting is empty`);
    ok = false;
  } else if (!fs.existsSync(rsvarsBat)) {
    output.appendLine(`error: rsvars.bat not found at ${rsvarsBat}`);
    ok = false;
  }
  if (!rsm2pdbExe || !fs.existsSync(rsm2pdbExe)) {
    output.appendLine(
      `error: rsm2pdb.exe not found at ${rsm2pdbExe || '(unset)'}\n` +
        `       Fix by either:\n` +
        `         (a) Ctrl+, → search "rsm2pdb" → set "Executable" to the\n` +
        `             absolute path of rsm2pdb.exe (e.g. ` +
        `C:\\Dev\\Src\\rsm2pdb\\build\\src\\rsm2pdb.exe)\n` +
        `         (b) build it from source: cd <rsm2pdb-repo> &&\n` +
        `             cmake --build build`,
    );
    // Offer a quick action.
    void vscode.window.showErrorMessage(
      'rsm2pdb.exe not found. Set its path in settings?',
      'Open Settings',
    ).then((choice) => {
      if (choice === 'Open Settings') {
        vscode.commands.executeCommand(
          'workbench.action.openSettings',
          'rsm2pdb.executable',
        );
      }
    });
    ok = false;
  }
  if (!ok) return undefined;

  output.appendLine(`bdsPath:    ${bdsPath}`);
  output.appendLine(`rsm2pdb:    ${rsm2pdbExe}`);
  return { bdsPath, rsvarsBat, rsm2pdbExe };
}

async function runMsbuild(
  output: vscode.OutputChannel,
  plan: BuildPlan,
  tools: ResolvedTools,
  diagAcc: Map<string, vscode.Diagnostic[]>,
  dprojDir: string,
): Promise<boolean> {
  const dprojName = path.basename(plan.dprojPath);
  // Delphi MSBuild requires the rsvars-set environment. Write a tiny
  // wrapper .bat so we don't have to fight Node's cmd.exe quoting (it
  // backslash-escapes quotes which cmd.exe doesn't understand).
  const script =
    `@echo off\r\n` +
    `call "${tools.rsvarsBat}" || exit /b 1\r\n` +
    `msbuild "${dprojName}" ^\r\n` +
    `  /t:${plan.target} ^\r\n` +
    `  /p:Config="${plan.configName}" ^\r\n` +
    `  /p:Platform=${plan.platform} ^\r\n` +
    `  /nologo /v:normal /clp:Summary;NoItemAndPropertyList\r\n`;
  output.appendLine(`\n--- msbuild wrapper ---\n${script.replace(/\r/g, '')}---`);
  return runViaBatchFile(script, dprojDir, output, 'msbuild', diagAcc, dprojDir);
}

async function runRsm2pdb(
  output: vscode.OutputChannel,
  plan: BuildPlan,
  tools: ResolvedTools,
): Promise<boolean> {
  const outDir = plan.settings.outputDir;
  const base = plan.settings.outputBaseName;
  const ext = plan.settings.outputExtension;
  const exePath = path.join(outDir, base + ext);
  const mapPath = path.join(outDir, base + '.map');
  const rsmPath = path.join(outDir, base + '.rsm');

  if (!fs.existsSync(exePath)) {
    output.appendLine(`error: build output not found: ${exePath}`);
    return false;
  }
  if (!fs.existsSync(mapPath)) {
    output.appendLine(`error: .map not found at ${mapPath} (is DCC_MapFile=3 set?)`);
    return false;
  }
  if (!fs.existsSync(rsmPath)) {
    output.appendLine(
      `warning: .rsm not found at ${rsmPath}; variables will be untyped.`,
    );
  }

  // In-place injection: input = output. rsm2pdb reads the whole input
  // into memory before writing, so this is safe.
  output.appendLine(
    `\n$ "${tools.rsm2pdbExe}" dwarf "${mapPath}" "${exePath}" "${exePath}"`,
  );
  return spawnAndPipe(
    tools.rsm2pdbExe,
    ['dwarf', mapPath, exePath, exePath],
    outDir,
    output,
    'rsm2pdb dwarf',
  );
}

/**
 * Run a snippet of cmd.exe batch by writing it to a temp .bat and
 * executing it directly. Avoids Node's spawn-on-Windows quote escaping.
 */
async function runViaBatchFile(
  script: string,
  cwd: string,
  output: vscode.OutputChannel,
  label?: string,
  diagAcc?: Map<string, vscode.Diagnostic[]>,
  dprojDir?: string,
): Promise<boolean> {
  const tmp = path.join(os.tmpdir(), `rsm2pdb-build-${process.pid}-${Date.now()}.bat`);
  fs.writeFileSync(tmp, script, { encoding: 'utf8' });
  try {
    return await spawnAndPipe('cmd.exe', ['/c', tmp], cwd, output, label,
                              diagAcc, dprojDir);
  } finally {
    try { fs.unlinkSync(tmp); } catch { /* ignore */ }
  }
}

function spawnAndPipe(
  command: string,
  args: string[],
  cwd: string,
  output: vscode.OutputChannel,
  label?: string,
  diagAcc?: Map<string, vscode.Diagnostic[]>,
  dprojDir?: string,
): Promise<boolean> {
  return new Promise((resolve) => {
    const child = cp.spawn(command, args, { cwd, shell: false });
    currentChild = child;
    currentChildLabel = label ?? path.basename(command);

    // Buffer stdout/stderr by lines so the diagnostic regex sees
    // complete messages. (Chunks can split mid-line otherwise.)
    let stdoutBuf = '';
    let stderrBuf = '';
    const handle = (chunk: string, isStderr: boolean) => {
      output.append(chunk);
      if (!diagAcc || !dprojDir) return;
      const ref = isStderr ? stderrBuf : stdoutBuf;
      const combined = ref + chunk;
      const idx = combined.lastIndexOf('\n');
      if (idx < 0) {
        if (isStderr) stderrBuf = combined; else stdoutBuf = combined;
        return;
      }
      const complete = combined.substring(0, idx);
      const leftover = combined.substring(idx + 1);
      if (isStderr) stderrBuf = leftover; else stdoutBuf = leftover;
      for (const line of complete.split(/\r?\n/)) {
        if (!line) continue;
        const d = parseDiagnostic(line);
        if (!d) continue;
        // Resolve file to an absolute path.
        const abs = path.isAbsolute(d.file)
          ? d.file
          : path.resolve(dprojDir, d.file);
        const range = new vscode.Range(
          d.line - 1, Math.max(0, d.col - 1),
          d.line - 1, Math.max(0, d.col - 1),
        );
        const diag = new vscode.Diagnostic(range, d.message, d.severity);
        diag.source = 'rsm2pdb';
        if (d.code) diag.code = d.code;
        const list = diagAcc.get(abs) ?? [];
        list.push(diag);
        diagAcc.set(abs, list);
      }
    };

    child.stdout.on('data', (b: Buffer) => handle(b.toString(), false));
    child.stderr.on('data', (b: Buffer) => handle(b.toString(), true));
    child.on('error', (e) => {
      currentChild = undefined;
      output.appendLine(`\nspawn error: ${e.message}`);
      resolve(false);
    });
    child.on('close', (code) => {
      currentChild = undefined;
      // Flush any trailing partial-line via the handler.
      if (stdoutBuf) handle('\n', false);
      if (stderrBuf) handle('\n', true);
      output.appendLine(`\n[exit ${code}]`);
      resolve(code === 0);
    });
  });
}

function publishDiagnostics(
  collection: vscode.DiagnosticCollection | undefined,
  acc: Map<string, vscode.Diagnostic[]>,
): void {
  if (!collection) return;
  collection.clear();
  for (const [filePath, diags] of acc) {
    collection.set(vscode.Uri.file(filePath), diags);
  }
}
