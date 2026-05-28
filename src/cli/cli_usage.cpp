#include "cli/cli.h"

#include <cstdio>

namespace rsm2pdb::cli {

int usage() {
    std::fputs(
        "rsm2pdb - Delphi RSM/MAP -> DWARF debug-info converter\n"
        "\n"
        "Usage:\n"
        "  rsm2pdb dump       <input.rsm | input.map>\n"
        "  rsm2pdb dwarf      <input.map> <input.exe> <output.exe>\n"
        "  rsm2pdb dwarf-emit <input.map> <out-dir>\n"
        "  rsm2pdb pdb        <input.map> <exe-in-place> <output.pdb>\n"
        "  rsm2pdb diff-procs  <input.map> <input.rsm>\n"
        "  rsm2pdb probe-procs   <input.map> <input.rsm>\n"
        "  rsm2pdb analyze-procs <input.map> <input.rsm>\n"
        "\n"
        "Notes:\n"
        "  'dump'       inspects an .rsm or .map (by extension).\n"
        "  'dwarf'      end-to-end: parse map, emit DWARF, inject\n"
        "               into the PE, write a new debuggable .exe.\n"
        "  'dwarf-emit' development: writes the .debug_* bytes\n"
        "               as separate files in <out-dir> for inspection.\n"
        "  'diff-procs' diagnostic: cross-checks map function symbols\n"
        "               against rsm procedure records (by VA). Reports\n"
        "               how many procs the rsm parser missed and whether\n"
        "               their names appear as raw bytes in the .rsm.\n"
        "  'probe-procs' diagnostic: for stratified missing procs, locate\n"
        "                the raw 0x28-record in .rsm bytes and dump hex\n"
        "                + decode (subtag / VA / trailer length).\n"
        "  'analyze-procs' single-pass aggregate: find every VA-confirmed\n"
        "                  proc record in the file, build histograms of\n"
        "                  subtag / trailer-length / sub-record-subtag\n"
        "                  to derive parsing rules.\n",
        stderr);
    return 2;
}

} // namespace rsm2pdb::cli
