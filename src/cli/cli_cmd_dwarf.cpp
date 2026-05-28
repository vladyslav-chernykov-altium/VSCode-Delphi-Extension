#include "cli/cli.h"
#include "cli/util.h"
#include "dwarf/dwarf_emitter.h"
#include "map/map_reader.h"
#include "model/model.h"
#include "pe/pe_injector.h"
#include "rsm/rsm_reader.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>

namespace rsm2pdb::cli {

int cmdDwarf(const std::string& map_path,
             const std::string& input_exe,
             const std::string& output_exe) {
    if (extLower(map_path) != "map") {
        std::fprintf(stderr, "error: first argument must be a .map file\n");
        return 1;
    }

    using clk = std::chrono::steady_clock;
    auto t = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };
    const auto t0 = clk::now();

    // 1. Parse .map
    rsm2pdb::map::Reader reader;
    if (!reader.open(map_path)) {
        std::fprintf(stderr, "error: %s\n", reader.error().c_str());
        return 1;
    }
    const auto t_map = clk::now();
    std::fprintf(stdout, "[%6.2fs] parsed %s (%zu segments, %zu publics, %zu line-tables)\n",
                 t(t0, t_map), map_path.c_str(),
                 reader.file().segments.size(),
                 reader.file().publics.size(),
                 reader.file().line_tables.size());

    // 2. Populate model::Module
    rsm2pdb::model::Module mod;
    rsm2pdb::map::populate(reader.file(), mod);
    const auto t_pop = clk::now();
    std::size_t total_syms = 0;
    for (const auto& cu : mod.units) total_syms += cu.symbols.size();
    std::fprintf(stdout, "[%6.2fs] populate -> %zu CUs, %zu symbols\n",
                 t(t_map, t_pop), mod.units.size(), total_syms);

    // 3. If a sibling .rsm exists, decorate variables with types.
    std::filesystem::path rsm_path = map_path;
    rsm_path.replace_extension(".rsm");
    if (std::filesystem::exists(rsm_path)) {
        const auto t_rsm_pre = clk::now();
        rsm2pdb::rsm::Reader rsm;
        if (rsm.open(rsm_path.string())) {
            const auto t_rsm_open = clk::now();
            std::fprintf(stdout, "[%6.2fs] rsm parsed (%zu primitives, %zu vars, %zu procs)\n",
                         t(t_rsm_pre, t_rsm_open),
                         rsm.primitives().size(),
                         rsm.variables().size(),
                         rsm.procedures().size());
            rsm2pdb::rsm::decorateTypes(rsm, mod);
            const auto t_dec = clk::now();
            std::fprintf(stdout, "[%6.2fs] decorateTypes done\n",
                         t(t_rsm_open, t_dec));
        } else {
            std::fprintf(stderr,
                "warning: failed to read %s (%s); variables will be untyped\n",
                rsm_path.string().c_str(), rsm.error().c_str());
        }
    }

    const auto t_pre_emit = clk::now();

    // 4. Emit DWARF sections
    rsm2pdb::dwarf::DwarfSections sections;
    std::string err;
    if (!rsm2pdb::dwarf::emit(mod, {}, sections, err)) {
        std::fprintf(stderr, "error: DWARF emit failed: %s\n", err.c_str());
        return 1;
    }
    const auto t_emit = clk::now();
    std::fprintf(stdout,
                 "[%6.2fs] DWARF emit: info=%zu abbrev=%zu line=%zu\n",
                 t(t_pre_emit, t_emit),
                 sections.debug_info.size(),
                 sections.debug_abbrev.size(),
                 sections.debug_line.size());

    // 5. Inject into PE and write output
    if (!rsm2pdb::pe::injectDwarfFile(input_exe, sections,
                                      output_exe, err)) {
        std::fprintf(stderr, "error: PE injection failed: %s\n",
                     err.c_str());
        return 1;
    }
    const auto t_inj = clk::now();
    std::fprintf(stdout, "[%6.2fs] PE injection done -> %s\n",
                 t(t_emit, t_inj), output_exe.c_str());
    std::fprintf(stdout, "[%6.2fs] TOTAL\n", t(t0, t_inj));
    return 0;
}

} // namespace rsm2pdb::cli
