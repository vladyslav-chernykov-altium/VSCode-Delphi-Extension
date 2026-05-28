#include "cli/cli.h"
#include "cli/util.h"
#include "dwarf/dwarf_emitter.h"
#include "map/map_reader.h"
#include "model/model.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace rsm2pdb::cli {

int cmdDwarfEmit(const std::string& map_path,
                 const std::string& out_dir) {
    if (extLower(map_path) != "map") {
        std::fprintf(stderr,
            "error: dwarf-emit input must be a .map file\n");
        return 1;
    }
    rsm2pdb::map::Reader reader;
    if (!reader.open(map_path)) {
        std::fprintf(stderr, "error: %s\n", reader.error().c_str());
        return 1;
    }
    rsm2pdb::model::Module mod;
    rsm2pdb::map::populate(reader.file(), mod);

    rsm2pdb::dwarf::DwarfSections sections;
    std::string err;
    if (!rsm2pdb::dwarf::emit(mod, {}, sections, err)) {
        std::fprintf(stderr, "error: dwarf emit failed: %s\n", err.c_str());
        return 1;
    }

    std::filesystem::create_directories(out_dir);
    auto writeSec = [&](const char* name,
                        const std::vector<std::uint8_t>& bytes) {
        auto p = std::filesystem::path(out_dir) / name;
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        std::fprintf(stdout, "  %-20s %zu bytes\n", name, bytes.size());
    };
    std::fprintf(stdout, "wrote DWARF sections to %s/\n", out_dir.c_str());
    writeSec(".debug_info",     sections.debug_info);
    writeSec(".debug_abbrev",   sections.debug_abbrev);
    writeSec(".debug_line",     sections.debug_line);
    writeSec(".debug_str",      sections.debug_str);
    writeSec(".debug_line_str", sections.debug_line_str);
    return 0;
}

} // namespace rsm2pdb::cli
