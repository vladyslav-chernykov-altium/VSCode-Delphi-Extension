// Fast "is this Make a no-op?" pre-check, run BEFORE invoking msbuild.
//
// Why this exists: Delphi's MSBuild integration always re-runs the
// linker on Make, even when no .pas changed. On large projects
// (AdvPCB-scale: thousands of units, hundreds of search paths) that
// alone costs ~30s. It also bumps the .exe mtime, which then
// invalidates our downstream rsm2pdb up-to-date check (mtime-based)
// and triggers a 50+s PDB regeneration. End result: a Make on
// genuinely unchanged sources costs the user ~90s.
//
// What we do here: walk every source file Delphi could possibly
// consume (DCC_UnitSearchPath dirs + MainSource + DCCReference items
// + the .dproj itself + rsm2pdb.exe binary) and check whether any
// of them is newer than the existing build output. If nothing is
// newer, the user's last build is still valid -- we skip msbuild
// AND rsm2pdb entirely and return success in ~1-3s instead of ~90s.
//
// Source-set semantics:
//   - .pas / .dpr / .dpk / .inc / .res / .rc files in each search
//     path dir (NON-recursive: Delphi search paths are flat per
//     entry; recursion would explode on AdvPCB's hundreds of dirs).
//   - Every DCCReference explicitly listed in the .dproj.
//   - MainSource.
//   - The .dproj file itself (catches config / setting edits).
//   - rsm2pdb.exe (catches "I just rebuilt the converter -> please
//     regenerate the PDB").
//
// Performance: parallel fs.promises.stat for all collected files.
// On AdvPCB (~thousands of sources across ~hundreds of search
// paths) this completes in 1-3s on Windows, vs the 30+57=87s of
// running msbuild+rsm2pdb pointlessly.

import * as fs from 'fs';
import * as path from 'path';

import type { DprojSettings } from './dprojParser';

export interface UpToDateInput {
  dprojPath: string;
  settings: DprojSettings;
  rsm2pdbExe: string;
}

export interface UpToDateResult {
  upToDate: boolean;
  reason: string;            // human-readable summary for the Output log
  filesScanned: number;
  scanMs: number;
}

const SOURCE_EXTS = new Set([
  '.pas', '.dpr', '.dpk', '.inc', '.res', '.rc',
]);

export async function isProjectUpToDate(
  input: UpToDateInput,
): Promise<UpToDateResult> {
  const t0 = Date.now();
  const exePath = input.settings.outputFullPath;

  // If there's no output to compare against, definitely not up-to-date.
  let exeMtime: number;
  try {
    exeMtime = (await fs.promises.stat(exePath)).mtimeMs;
  } catch {
    return {
      upToDate: false,
      reason: `${path.basename(exePath)} doesn't exist yet`,
      filesScanned: 0,
      scanMs: Date.now() - t0,
    };
  }

  // --- Cheap upfront checks: .dproj, rsm2pdb.exe ---
  const cheapChecks: Array<{ p: string; label: string }> = [
    { p: input.dprojPath, label: path.basename(input.dprojPath) },
    { p: input.rsm2pdbExe, label: 'rsm2pdb.exe' },
  ];
  for (const c of cheapChecks) {
    try {
      const m = (await fs.promises.stat(c.p)).mtimeMs;
      if (m > exeMtime) {
        return {
          upToDate: false,
          reason: `${c.label} is newer than ${path.basename(exePath)}`,
          filesScanned: 0,
          scanMs: Date.now() - t0,
        };
      }
    } catch {
      // missing -> ignore; cheapChecks are best-effort.
    }
  }

  // --- Collect file list ---
  // Deduplicate dirs (same dir can appear in multiple search-path
  // entries on big projects).
  const dirs = new Set<string>();
  for (const d of input.settings.unitSearchPaths) {
    if (d) dirs.add(path.normalize(d));
  }

  // List files in every search-path dir, in parallel.
  const fileLists = await Promise.all(
    Array.from(dirs).map((d) => listSourceFiles(d)),
  );
  const files = new Set<string>();
  for (const lst of fileLists) {
    for (const f of lst) files.add(f);
  }

  // Add explicit DCCReference items + MainSource.
  for (const ref of input.settings.dccReferences) {
    files.add(path.normalize(ref));
  }
  if (input.settings.mainSource) {
    const dpr = path.isAbsolute(input.settings.mainSource)
      ? input.settings.mainSource
      : path.resolve(path.dirname(input.dprojPath),
                     input.settings.mainSource);
    files.add(path.normalize(dpr));
  }

  // --- Parallel stat ---
  // Promise.all over potentially thousands of files. Node funnels
  // these through libuv's thread pool; on Windows that's still
  // markedly faster than sequential stat thanks to overlap with
  // path resolution. We early-exit on the first newer-than-exe
  // hit so on the common "one file edited" case we get back
  // quickly.
  const fileArr = Array.from(files);
  let scanned = 0;
  let newest: { f: string; mtime: number } | null = null;

  // Batch in chunks so we can short-circuit. A pure Promise.all
  // would force the full scan even when we already know the
  // result. Chunks of 256 balance parallelism against early exit.
  const CHUNK = 256;
  for (let i = 0; i < fileArr.length; i += CHUNK) {
    const chunk = fileArr.slice(i, i + CHUNK);
    const stats = await Promise.all(chunk.map(async (f) => {
      try {
        const s = await fs.promises.stat(f);
        return { f, mtime: s.mtimeMs };
      } catch {
        return null;
      }
    }));
    for (const s of stats) {
      if (!s) continue;
      scanned++;
      if (s.mtime > exeMtime) {
        return {
          upToDate: false,
          reason: `${path.basename(s.f)} is newer than ${path.basename(exePath)}`,
          filesScanned: scanned,
          scanMs: Date.now() - t0,
        };
      }
      if (!newest || s.mtime > newest.mtime) newest = s;
    }
  }

  return {
    upToDate: true,
    reason: `${scanned} source file(s) scanned, all older than ` +
            `${path.basename(exePath)}` +
            (newest ? ` (newest: ${path.basename(newest.f)})` : ''),
    filesScanned: scanned,
    scanMs: Date.now() - t0,
  };
}

async function listSourceFiles(dir: string): Promise<string[]> {
  let entries: fs.Dirent[];
  try {
    entries = await fs.promises.readdir(dir, { withFileTypes: true });
  } catch {
    return [];
  }
  const out: string[] = [];
  for (const e of entries) {
    if (!e.isFile()) continue;
    const ext = path.extname(e.name).toLowerCase();
    if (!SOURCE_EXTS.has(ext)) continue;
    out.push(path.join(dir, e.name));
  }
  return out;
}
