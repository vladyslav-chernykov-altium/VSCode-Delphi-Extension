#include "cli/cli.h"
#include "cli/util.h"
#include "map/map_reader.h"
#include "rsm/rsm_reader.h"

#include <cstdio>

namespace rsm2pdb::cli {

int cmdDump(const std::string& path) {
    const auto ext = extLower(path);
    if (ext == "map") {
        rsm2pdb::map::Reader reader;
        if (!reader.open(path)) {
            std::fprintf(stderr, "error: %s\n", reader.error().c_str());
            return 1;
        }
        reader.dump(stdout);
        return 0;
    }
    if (ext == "rsm") {
        rsm2pdb::rsm::Reader reader;
        if (!reader.open(path)) {
            std::fprintf(stderr, "error: %s\n", reader.error().c_str());
            return 1;
        }
        reader.dump(stdout);
        return 0;
    }
    std::fprintf(stderr,
                 "error: unrecognized file extension '%s' "
                 "(want .map or .rsm)\n",
                 ext.c_str());
    return 1;
}

} // namespace rsm2pdb::cli
