#pragma once

// Public surface of the CLI subcommand layer. main.cpp dispatches
// argv[1] to one of these; the implementations live in
// cli/cli_cmd_*.cpp. Keeping a single header avoids a per-file
// trail of mini-headers; each entry point is a small free function
// that returns a process-exit-code.

#include <string>

namespace rsm2pdb::cli {

// Print usage text to stderr and return 2 (conventional "bad
// invocation" exit code).
int usage();

// `rsm2pdb dump <input.rsm | input.map>` -- dispatches on the
// file extension and runs the matching Reader's dump() method.
int cmdDump(const std::string& path);

// `rsm2pdb diff-procs <map> <rsm>` -- diagnostic. VA-cross-checks
// the .map function symbols against parsed RSM proc records;
// reports matched / missing / orphans + a stride-sampled list of
// the missing ones with raw-name-substring probes.
int cmdDiffProcs(const std::string& map_path,
                 const std::string& rsm_path);

// `rsm2pdb probe-procs <map> <rsm>` -- for a stratified sample of
// missing-in-rsm functions, locate the raw 0x28 proc record in
// the .rsm bytes and dump hex + decoded structure (subtag, VA,
// trailer length, sub-record subtag).
int cmdProbeProcs(const std::string& map_path,
                  const std::string& rsm_path);

// `rsm2pdb analyze-procs <map> <rsm>` -- single-pass aggregate
// that finds every VA-confirmed proc record in the file and
// builds histograms of (proc subtag / trailer length / sub-record
// subtag) over the whole .rsm. Used to derive / verify the
// scanner's permissive subtag set.
int cmdAnalyzeProcs(const std::string& map_path,
                    const std::string& rsm_path);

// `rsm2pdb dwarf-emit <map> <out-dir>` -- development helper.
// Writes the .debug_* bytes as separate files for inspection
// (objdump / readelf / hex). Doesn't touch any PE.
int cmdDwarfEmit(const std::string& map_path,
                 const std::string& out_dir);

// `rsm2pdb dwarf <map> <input.exe> <output.exe>` -- end-to-end:
// parse map (+ optional sibling .rsm for types), emit DWARF v5
// sections, inject as fresh .debug_* sections into a copy of
// input.exe, write to output.exe.
int cmdDwarf(const std::string& map_path,
             const std::string& input_exe,
             const std::string& output_exe);

// `rsm2pdb pdb <map> <exe-in-place> <pdb> [--src-search <dir> ...]`
// -- end-to-end: parse PE + map + sibling .rsm, compose a typed
// PDB via the LLVM PDBFileBuilder + GSI / DBI / TPI streams,
// inject an RSDS Debug Directory entry pointing at the PDB into
// exe-in-place. The PDB output path is independent; the EXE is
// rewritten with the new debug-dir pointer.
int cmdPdb(int argc, char** argv);

} // namespace rsm2pdb::cli
