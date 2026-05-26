// Parser for Delphi MSBuild project files (.dproj).
//
// A .dproj is XML with the MSBuild element schema. Build settings are
// scattered across PropertyGroup elements with $(Config) / $(Platform)
// conditions; settings cascade via a <CfgParent> chain. Resolving a
// concrete (Config, Platform) tuple to a flat settings dict requires
// walking that chain and overlaying.
//
// Real-world observations from a 2786-line .dproj (Altium AdvPCB):
//   - 11 named Configs, each a Cfg_N internally (Cfg_2..Cfg_16).
//   - Each Config has Win32 and Win64 PropertyGroup variants.
//   - Inheritance: Cfg_11 -> Base; Cfg_11_Win64 -> Cfg_11 -> Base_Win64.
//   - Per-config-platform PropertyGroups carry the interesting settings
//     (Debugger_HostApplication, DCC_ExeOutput, DCC_MapFile, ...).
//   - Settings reference environment variables via $(NAME), e.g.
//     $(ALTIUM64_HOME).
//
// This parser does NOT use an XML library — Delphi .dproj XML is
// well-formed but verbose, and the patterns we care about are
// trivially regex-able. Keeping zero deps makes the extension lean.

import * as fs from 'fs';
import * as path from 'path';

export type Platform = 'Win64' | 'Win32';
export type AppType  = 'Library' | 'Application' | 'Package';

export interface DprojConfig {
  /** Human-friendly config name, e.g. "Debug :: Built-In". */
  name: string;
  /** Internal Cfg_N id, e.g. "Cfg_11". */
  cfgId: string;
  platform: Platform;
}

export interface DprojSettings {
  /** Resolved settings (env vars expanded; inheritance chain flattened). */
  appType: AppType;
  mainSource: string;          // e.g. "AdvPCB.dpr"
  projectName: string;         // derived from .dproj filename

  outputDir: string;           // DCC_ExeOutput, resolved
  outputBaseName: string;      // derived from MainSource (no extension)
  outputExtension: string;     // ".dll" for Library, ".exe" otherwise
  outputFullPath: string;      // outputDir / outputBaseName + outputExtension

  /** For Library projects, the host EXE used as a debugger target. */
  hostApplication: string | null;
  debuggerCwd: string | null;
  debuggerRunParams: string | null;

  /** Numeric DCC_* values; defaults applied when absent. */
  mapFile: number;             // 0 (off) .. 3 (detailed); default 0
  debugInformation: number;    // 0 (off) / 1 (on)
  localDebugSymbols: boolean;
  debugInfoInExe: boolean;     // critical: if true, PE injection may fail
  optimize: boolean;
  remoteDebug: boolean;        // controls .rsm generation
  useMSBuildExternally: boolean;

  /** Raw conditional defines combined across the chain. */
  define: string;
}

/** Returns all (Config, Platform) tuples advertised by a .dproj. */
export function listConfigs(dprojPath: string): DprojConfig[] {
  const xml = fs.readFileSync(dprojPath, 'utf8');

  // Map Cfg_N -> human name via the conditional PropertyGroup headers.
  // Pattern:  <PropertyGroup Condition="'$(Config)'=='Foo' or '$(Cfg_N)'!=''">
  const cfgNameByCfgId: Record<string, string> = {};
  const reNamed =
    /<PropertyGroup\s+Condition\s*=\s*"'\$\(Config\)'=='([^']+)'\s+or\s+'\$\(Cfg_(\d+)\)'!=''"\s*>/g;
  for (let m: RegExpExecArray | null; (m = reNamed.exec(xml)); ) {
    cfgNameByCfgId[`Cfg_${m[2]}`] = m[1];
  }

  // Find every Cfg_N_PlatformX PropertyGroup; that's the canonical
  // proof of an actual buildable (config, platform).
  const reCfgPlat =
    /<PropertyGroup\s+Condition\s*=\s*"\([^"]*'\$\(Cfg_(\d+)\)'=='true'\)[^"]*'\$\(Cfg_\d+_(Win32|Win64)\)'/g;
  const seen = new Set<string>();
  const out: DprojConfig[] = [];
  for (let m: RegExpExecArray | null; (m = reCfgPlat.exec(xml)); ) {
    const cfgId = `Cfg_${m[1]}`;
    const platform = m[2] as Platform;
    const key = `${cfgId}::${platform}`;
    if (seen.has(key)) continue;
    seen.add(key);
    out.push({
      cfgId,
      platform,
      name: cfgNameByCfgId[cfgId] ?? cfgId,
    });
  }
  // Stable order: Win64 first within each Cfg, ascending Cfg_N.
  out.sort((a, b) => {
    const an = parseInt(a.cfgId.slice(4), 10);
    const bn = parseInt(b.cfgId.slice(4), 10);
    return an - bn || (a.platform === 'Win64' ? -1 : 1);
  });
  return out;
}

/**
 * Resolve a concrete (Config, Platform) into a flat DprojSettings dict.
 * Walks the inheritance chain (Cfg_N_Plat -> Cfg_N -> Base_Plat -> Base)
 * and overlays settings, then expands ${...} and $(...) env vars.
 */
export function resolveSettings(
  dprojPath: string,
  cfgId: string,
  platform: Platform,
): DprojSettings {
  const xml = fs.readFileSync(dprojPath, 'utf8');
  const dprojDir = path.dirname(dprojPath);

  // -------- 1. Collect raw key/value pairs per PropertyGroup --------
  const groups = extractPropertyGroups(xml);

  // -------- 2. Determine which groups apply, in inheritance order --------
  // The chain we apply (most-general to most-specific):
  //   Base, Base_<Platform>, <CfgId>, <CfgId>_<Platform>
  const wanted = [
    'Base',
    `Base_${platform}`,
    cfgId,
    `${cfgId}_${platform}`,
  ];

  const merged: Record<string, string> = {};
  for (const scope of wanted) {
    for (const g of groups) {
      if (g.scope === scope) {
        for (const [k, v] of Object.entries(g.props)) merged[k] = v;
      }
    }
  }

  // -------- 3. MainSource + project-level fixed bits --------
  // These live in the unconditional <PropertyGroup> at the top.
  const topProps = groups.find((g) => g.scope === '_top')?.props ?? {};
  const mainSource = topProps.MainSource ?? path.basename(dprojPath, '.dproj') + '.dpr';
  const appTypeRaw = topProps.AppType ?? 'Application';
  const appType: AppType =
    appTypeRaw === 'Library' ? 'Library' :
    appTypeRaw === 'Package' ? 'Package' :
    'Application';

  // -------- 4. Resolve env vars in string-valued settings --------
  const resolve = (s: string | undefined): string =>
    s === undefined ? '' : expandVars(s, { Platform: platform });

  const outputDir = resolve(merged.DCC_ExeOutput) ||
                    resolve(merged.DCC_AsmOutput) ||
                    dprojDir;
  const outputBaseName = path.basename(mainSource, path.extname(mainSource));
  const outputExtension =
    appType === 'Library' ? '.dll' :
    appType === 'Package' ? '.bpl' : '.exe';

  return {
    appType,
    mainSource,
    projectName: outputBaseName,

    outputDir,
    outputBaseName,
    outputExtension,
    outputFullPath: path.join(outputDir, outputBaseName + outputExtension),

    hostApplication: resolve(merged.Debugger_HostApplication) || null,
    debuggerCwd: resolve(merged.Debugger_CWD) || null,
    debuggerRunParams: resolve(merged.Debugger_RunParams) || null,

    mapFile: parseInt(merged.DCC_MapFile ?? '0', 10),
    debugInformation: parseInt(merged.DCC_DebugInformation ?? '0', 10),
    localDebugSymbols: merged.DCC_LocalDebugSymbols === 'true',
    debugInfoInExe:
      merged.DCC_DebugInfoInExe === undefined
        ? true  // Delphi defaults this ON when not set explicitly
        : merged.DCC_DebugInfoInExe === 'true',
    optimize:
      merged.DCC_Optimize === undefined
        ? true
        : merged.DCC_Optimize === 'true',
    remoteDebug: merged.DCC_RemoteDebug === 'true',
    useMSBuildExternally: merged.DCC_UseMSBuildExternally === 'true',

    define: resolve(merged.DCC_Define),
  };
}

// ----------------------- internal helpers -----------------------

interface RawGroup {
  /** "_top" for the unconditional first group; otherwise "Base",
   *  "Base_Win64", "Cfg_11", "Cfg_11_Win64", ... */
  scope: string;
  props: Record<string, string>;
}

function extractPropertyGroups(xml: string): RawGroup[] {
  // First, the unconditional <PropertyGroup> at the very top.
  const groups: RawGroup[] = [];
  const reTop = /<PropertyGroup>\s*([\s\S]*?)<\/PropertyGroup>/;
  const topMatch = reTop.exec(xml);
  if (topMatch) {
    groups.push({ scope: '_top', props: parseKeyValues(topMatch[1]) });
  }

  // Then every conditional one.
  const reCond =
    /<PropertyGroup\s+Condition\s*=\s*"([^"]+)"\s*>\s*([\s\S]*?)<\/PropertyGroup>/g;
  for (let m: RegExpExecArray | null; (m = reCond.exec(xml)); ) {
    const cond = m[1];
    const body = m[2];
    const scope = scopeOfCondition(cond);
    if (!scope) continue;
    groups.push({ scope, props: parseKeyValues(body) });
  }
  return groups;
}

/**
 * Translate a PropertyGroup Condition string into one of our scope
 * tokens. Returns null when we can't classify it (extra-safe: the
 * extension just ignores groups we don't recognise).
 */
function scopeOfCondition(cond: string): string | null {
  // Match shapes we know from Delphi's .dproj XML.
  if (/'\$\(Base_Win32\)'!=''/.test(cond)) return 'Base_Win32';
  if (/'\$\(Base_Win64\)'!=''/.test(cond)) return 'Base_Win64';
  if (/'\$\(Base\)'!=''$/.test(cond)) return 'Base';
  // Cfg_N_Plat first (more specific check)
  let m = cond.match(/'\$\(Cfg_(\d+)_(Win32|Win64)\)'!=''/);
  if (m) return `Cfg_${m[1]}_${m[2]}`;
  m = cond.match(/'\$\(Cfg_(\d+)\)'!=''$/);
  if (m) return `Cfg_${m[1]}`;
  m = cond.match(/'\$\(Cfg_(\d+)\)'!=''/);
  if (m) return `Cfg_${m[1]}`;
  return null;
}

function parseKeyValues(body: string): Record<string, string> {
  const out: Record<string, string> = {};
  const re = /<([A-Za-z_][\w.]*?)>([\s\S]*?)<\/\1>/g;
  for (let m: RegExpExecArray | null; (m = re.exec(body)); ) {
    out[m[1]] = m[2].trim();
  }
  return out;
}

/**
 * Expand $(NAME) and ${NAME} references using:
 *   - process.env
 *   - the supplied `extra` overlay (for vars like $(Platform))
 * Unknown vars are left in place so the caller can spot them.
 */
function expandVars(input: string, extra: Record<string, string>): string {
  return input.replace(/\$[\{\(]([A-Za-z_][\w.]*)[\}\)]/g, (_, name) => {
    if (name in extra) return extra[name];
    const env = process.env[name];
    if (env !== undefined) return env;
    return `$(${name})`;   // leave unresolved
  });
}
