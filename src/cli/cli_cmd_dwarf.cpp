#include "cli/cli.h"
#include "cli/util.h"
#include "compose/frame.h"
#include "dwarf/dwarf_emitter.h"
#include "map/map_reader.h"
#include "model/model.h"
#include "pe/pe_injector.h"
#include "rsm/rsm_reader.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

    // 3. If a sibling .rsm exists, decorate variables with types
    //    (legacy size-based path for globals + initial pass for
    //    procs) then re-resolve every procedure's frame layout via
    //    compose::resolveFunction. The compose pass overwrites
    //    LocalVar::stack_offset with the rbp-relative real offset
    //    that the new Delphi-x64 frame interpretation computes,
    //    while keeping the type assignments decorateTypes already
    //    made (since model::LocalVar lacks a kind/byte_size field
    //    of its own).
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

            // Slurp the input PE so compose::resolveFunction can read
            // each function's prologue + body to recover sub_rsp and
            // extra_pushes. If we can't read the PE we leave the
            // legacy stack_offsets in place (DWARF emitter will then
            // produce wrong addresses, but that mirrors the pre-stage-3
            // behaviour rather than regressing globals' types).
            std::vector<std::uint8_t> pe_bytes;
            {
                std::ifstream f(input_exe, std::ios::binary);
                if (f) {
                    std::ostringstream ss; ss << f.rdbuf();
                    const std::string& s = ss.str();
                    pe_bytes.assign(s.begin(), s.end());
                }
            }
            if (!pe_bytes.empty()
                && pe_bytes.size() >= sizeof(IMAGE_DOS_HEADER)) {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(
                    pe_bytes.data());
                if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                        pe_bytes.data() + dos->e_lfanew);
                    const auto* pe_secs = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
                        reinterpret_cast<const std::uint8_t*>(nt)
                            + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
                            + nt->FileHeader.SizeOfOptionalHeader);
                    const std::uint64_t image_base =
                        nt->OptionalHeader.ImageBase;

                    // Collect all Function symbols sorted by VA so we
                    // can compute each function's size as next-VA gap.
                    std::vector<rsm2pdb::model::Symbol*> all_funcs;
                    for (auto& cu : mod.units) {
                        for (auto& s : cu.symbols) {
                            if (s.kind == rsm2pdb::model::SymbolKind::Function) {
                                all_funcs.push_back(&s);
                            }
                        }
                    }
                    std::sort(all_funcs.begin(), all_funcs.end(),
                        [](const rsm2pdb::model::Symbol* a,
                           const rsm2pdb::model::Symbol* b) {
                            return a->address < b->address;
                        });

                    const auto marker_sizes =
                        rsm2pdb::compose::buildMarkerSizes(rsm);

                    // Helper: find PE section containing an RVA -> slice
                    // of PE bytes for [VA, VA + size).
                    auto sliceForVa = [&](std::uint64_t va,
                                          std::uint32_t size)
                        -> std::pair<const std::uint8_t*, std::size_t>
                    {
                        if (va < image_base) return {nullptr, 0};
                        const std::uint64_t rva = va - image_base;
                        for (std::uint16_t k = 0;
                             k < nt->FileHeader.NumberOfSections; ++k) {
                            const auto& sec = pe_secs[k];
                            const std::uint32_t vsz =
                                std::max(sec.Misc.VirtualSize,
                                         sec.SizeOfRawData);
                            if (rva >= sec.VirtualAddress &&
                                rva <  static_cast<std::uint64_t>(
                                           sec.VirtualAddress) + vsz)
                            {
                                const std::size_t fn_fo =
                                    sec.PointerToRawData
                                        + (rva - sec.VirtualAddress);
                                if (fn_fo + size <= pe_bytes.size()) {
                                    return {pe_bytes.data() + fn_fo,
                                            static_cast<std::size_t>(size)};
                                }
                                break;
                            }
                        }
                        return {nullptr, 0};
                    };

                    std::size_t composed = 0;
                    for (std::size_t i = 0; i < all_funcs.size(); ++i) {
                        auto* sym = all_funcs[i];
                        const auto* pr = rsm.findProcedureAt(sym->address);
                        if (!pr) continue;
                        const std::uint64_t next_va =
                            (i + 1 < all_funcs.size())
                            ? all_funcs[i + 1]->address
                            : sym->address + 1024;
                        const std::uint32_t fn_size =
                            (next_va > sym->address)
                            ? static_cast<std::uint32_t>(
                                  next_va - sym->address)
                            : 256;
                        const auto [code, code_len] =
                            sliceForVa(sym->address, fn_size);
                        const auto rf =
                            rsm2pdb::compose::resolveFunction(
                                *pr, code, code_len, marker_sizes);

                        // Rebuild params/locals from rf.vars, but
                        // keep the types decorateTypes assigned --
                        // match by position in the proc record
                        // (compose preserves order: static_link
                        // first, then params, then locals).
                        std::vector<rsm2pdb::model::LocalVar> new_params;
                        std::vector<rsm2pdb::model::LocalVar> new_locals;
                        std::size_t pi = 0, li = 0;
                        for (const auto& rv : rf.vars) {
                            // Skip the synthesised __frame_outer__
                            // static-link slot: DWARF's
                            // DW_TAG_formal_parameter requires a
                            // DW_AT_type referent and we don't have
                            // a TypeId for a raw frame pointer.
                            // PDB handles it separately via its
                            // void* fallback. Nested-function
                            // outer-frame visibility through gdb is
                            // a future TPI-struct-equivalent
                            // exercise.
                            if (rv.is_static_link) continue;
                            rsm2pdb::model::LocalVar lv;
                            lv.name         = rv.name;
                            lv.stack_offset = rv.rbp_offset;
                            if (rv.is_param) {
                                if (pi < sym->params.size()) {
                                    lv.type = sym->params[pi].type;
                                }
                                ++pi;
                            } else {
                                if (li < sym->locals.size()) {
                                    lv.type = sym->locals[li].type;
                                }
                                ++li;
                            }
                            (rv.is_param ? new_params : new_locals)
                                .push_back(std::move(lv));
                        }
                        sym->params = std::move(new_params);
                        sym->locals = std::move(new_locals);
                        ++composed;
                    }
                    const auto t_compose = clk::now();
                    std::fprintf(stdout,
                        "[%6.2fs] compose: %zu procs re-resolved via Delphi-x64 frame\n",
                        t(t_dec, t_compose), composed);
                } else {
                    std::fprintf(stderr,
                        "warning: input PE has bad DOS magic; "
                        "skipping compose-based frame re-resolution\n");
                }
            } else {
                std::fprintf(stderr,
                    "warning: couldn't read %s; "
                    "DWARF locals will use legacy stack offsets\n",
                    input_exe.c_str());
            }
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
