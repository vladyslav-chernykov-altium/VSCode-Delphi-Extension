// rsm2pdb -- top-level dispatch only. Each subcommand lives in its own
// translation unit under cli/. Keeping main.cpp slim makes the
// `argv[1] -> cmd` mapping the entire spec of what the binary does.

#include "cli/cli.h"
#include "cli/util.h"

#include <cstring>
#include <string_view>

int main(int argc, char** argv) {
    using namespace rsm2pdb::cli;

    if (argc < 2) return usage();
    const std::string_view cmd = argv[1];

    if (cmd == "dump" && argc == 3) {
        return cmdDump(argv[2]);
    }
    if (cmd == "diff-procs" && argc == 4) {
        return cmdDiffProcs(argv[2], argv[3]);
    }
    if (cmd == "probe-procs" && argc == 4) {
        return cmdProbeProcs(argv[2], argv[3]);
    }
    if (cmd == "analyze-procs" && argc == 4) {
        return cmdAnalyzeProcs(argv[2], argv[3]);
    }
    if (cmd == "dwarf-emit" && argc == 4) {
        return cmdDwarfEmit(argv[2], argv[3]);
    }
    if (cmd == "dwarf" && argc == 5) {
        return cmdDwarf(argv[2], argv[3], argv[4]);
    }
    if (cmd == "pdb" && argc >= 5) {
        return cmdPdb(argc, argv);
    }

    return usage();
}
