#include "cli/cli.h"
#include "cli/source_path.h"
#include "cli/util.h"
#include "compose/frame.h"
#include "map/map_reader.h"
#include "pdb/pdb_writer.h"
#include "pe/pe_pdb_injector.h"
#include "rsm/rsm_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace rsm2pdb::cli {

int cmdPdb(int argc, char** argv) {
    // Caller has already verified argv[1] == "pdb" and argc >= 5.
    const std::string map_path(argv[2]);
    const std::string input_exe(argv[3]);
    const std::string& output_exe = input_exe;  // in-place injection
    const std::string output_pdb(argv[4]);
    std::vector<std::string> src_search_dirs;
    bool include_rtl = false;   // default: strip RTL / framework symbols
    for (int i = 5; i < argc; ++i) {
        if (std::strcmp(argv[i], "--src-search") == 0 && i + 1 < argc) {
            src_search_dirs.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--include-rtl") == 0) {
            include_rtl = true;
        } else {
            std::fprintf(stderr, "error: unknown pdb arg: %s\n", argv[i]);
            return 1;
        }
    }
    if (extLower(map_path) != "map") {
        std::fprintf(stderr, "error: first argument must be a .map file\n");
        return 1;
    }

    // Predicate: does this Pascal-qualified name (unit or unit-dot-
    // member, e.g. "System.SysUtils.Format") belong to a Delphi
    // built-in framework unit we have no proper debug info for and
    // therefore have no reason to publish in the PDB? Match prefixes
    // anchored at the start of the qualified name; we accept both
    // bare-unit ("System") and dotted-member ("System.X.Y") forms.
    //
    // Coverage = canonical Delphi-shipped frameworks. Third-party
    // libraries (Indy variants are sometimes called Idxxx) stay
    // visible -- those are normally what a user actually wants when
    // stepping into someone else's code.
    auto isRtlQName = [](std::string_view q) -> bool {
        static constexpr std::string_view roots[] = {
            "System",  "SysInit", "Winapi",   "Vcl",
            "Fmx",     "Soap",    "Datasnap", "Data",
            "Inet",    "IBX",     "REST",     "Bde",
            "IndySystem",
        };
        for (auto r : roots) {
            if (q.size() < r.size()) continue;
            if (q.compare(0, r.size(), r) != 0) continue;
            // Bare unit name match or dotted continuation.
            if (q.size() == r.size()) return true;
            if (q[r.size()] == '.') return true;
        }
        return false;
    };
    if (!include_rtl) {
        std::fputs("rtl filter: ON (use --include-rtl to keep "
                   "System.* / SysInit / Winapi.* / Vcl.* / ...)\n",
                   stdout);
    }

    // Per-phase timings; AdvPCB scale (~88 MB .text / ~500 MB .rsm)
    // makes individual phases run for many seconds, so emit
    // [N.NNs] timestamps the moment each phase starts/finishes.
    using clk = std::chrono::steady_clock;
    auto t = [](clk::time_point a, clk::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    };
    const auto t0 = clk::now();
    auto stamp = [&](const char* tag) {
        std::fprintf(stdout, "[%6.2fs] %s\n",
                     t(t0, clk::now()), tag);
        std::fflush(stdout);
    };
    stamp("phase: read PE");

    // 1. Parse the input PE to recover its section headers. Both
    //    the SectionMap in the PDB and the public symbols' segment
    //    + offset coordinates are derived from these.
    std::vector<std::uint8_t> pe_bytes;
    {
        std::ifstream f(input_exe, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "error: cannot open input PE: %s\n",
                         input_exe.c_str());
            return 1;
        }
        std::ostringstream ss; ss << f.rdbuf();
        const std::string& s = ss.str();
        pe_bytes.assign(s.begin(), s.end());
    }
    if (pe_bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        std::fprintf(stderr, "error: input PE too small\n");
        return 1;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pe_bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        std::fprintf(stderr, "error: input is not a PE\n");
        return 1;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        pe_bytes.data() + dos->e_lfanew);
    const auto* pe_secs = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(nt)
            + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
            + nt->FileHeader.SizeOfOptionalHeader);

    std::vector<rsm2pdb::pdb::CoffSection> coff_sections;
    coff_sections.reserve(nt->FileHeader.NumberOfSections);
    for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = pe_secs[i];
        rsm2pdb::pdb::CoffSection cs;
        cs.name.assign(reinterpret_cast<const char*>(s.Name),
                       strnlen(reinterpret_cast<const char*>(s.Name), 8));
        cs.virtual_size        = s.Misc.VirtualSize;
        cs.virtual_address     = s.VirtualAddress;
        cs.size_of_raw_data    = s.SizeOfRawData;
        cs.pointer_to_raw_data = s.PointerToRawData;
        cs.characteristics     = s.Characteristics;
        coff_sections.push_back(std::move(cs));
    }

    // 2. Parse the .map for publics.
    stamp("phase: parse .map");
    rsm2pdb::map::Reader reader;
    if (!reader.open(map_path)) {
        std::fprintf(stderr, "error: %s\n", reader.error().c_str());
        return 1;
    }
    const auto& mf = reader.file();
    std::fprintf(stdout,
        "        .map: %zu segments, %zu publics, %zu line-tables\n",
        mf.segments.size(), mf.publics.size(), mf.line_tables.size());
    std::fflush(stdout);

    // Build a segment-id -> CODE/DATA classifier from the .map.
    std::unordered_map<std::uint16_t, bool> seg_is_code;
    for (const auto& seg : mf.segments) {
        seg_is_code[seg.id] = (seg.klass == "CODE");
    }

    // Translate (.map segment, .map offset) into (PE section index,
    // PE-section-relative offset) by RVA. The .map's segment list
    // and PE's section list aren't 1:1 (PE adds .rdata, .pdata,
    // .reloc, ...) so we anchor via RVA = seg.start_va + offset.
    // .map segment.start_va is an absolute VA (image_base + RVA).
    // Convert to RVA so it lines up with PE section VirtualAddress.
    const std::uint64_t image_base = nt->OptionalHeader.ImageBase;
    auto rvaOfPublic = [&](const rsm2pdb::map::Public& p) -> std::uint64_t {
        const auto* seg = mf.findSegment(p.segment_id);
        if (!seg) return 0;
        const std::uint64_t va = seg->start_va + p.segment_offset;
        return va >= image_base ? va - image_base : va;
    };
    rsm2pdb::pdb::PdbInputs inputs;
    inputs.sections = std::move(coff_sections);

    auto findPeSection = [&](std::uint64_t rva) -> std::uint16_t {
        for (std::uint16_t i = 0; i < inputs.sections.size(); ++i) {
            const auto& s = inputs.sections[i];
            if (rva >= s.virtual_address &&
                rva <  static_cast<std::uint64_t>(s.virtual_address) +
                       std::max(s.virtual_size, s.size_of_raw_data)) {
                return static_cast<std::uint16_t>(i + 1);
            }
        }
        return 0;
    };

    inputs.age = 1;
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (std::size_t i = 0; i < inputs.guid.size(); i += 8) {
        std::uint64_t r = gen();
        std::size_t take = std::min<std::size_t>(8, inputs.guid.size() - i);
        std::memcpy(inputs.guid.data() + i, &r, take);
    }

    std::size_t skipped = 0;
    std::size_t skipped_rtl = 0;
    inputs.publics.reserve(mf.publics.size());
    for (const auto& p : mf.publics) {
        if (!include_rtl && isRtlQName(p.name)) { ++skipped_rtl; continue; }
        const std::uint64_t rva = rvaOfPublic(p);
        const std::uint16_t seg_idx = findPeSection(rva);
        if (seg_idx == 0) { ++skipped; continue; }
        const auto& pe_sec = inputs.sections[seg_idx - 1];
        rsm2pdb::pdb::PublicSymbol ps;
        ps.name        = p.name;
        ps.segment     = seg_idx;
        ps.offset      = static_cast<std::uint32_t>(rva - pe_sec.virtual_address);
        ps.is_function = seg_is_code[p.segment_id];
        inputs.publics.push_back(std::move(ps));
    }
    std::fprintf(stdout,
                 "publics: %zu emitted, %zu no-PE-section, %zu RTL-filtered\n",
                 inputs.publics.size(), skipped, skipped_rtl);

    // Optional: parse sibling .rsm for procedure records (params +
    // locals). When present, each function's locals[] gets
    // populated and we emit S_REGREL32 records inside its proc.
    rsm2pdb::rsm::Reader rsm_reader;
    bool have_rsm = false;
    {
        std::filesystem::path rp = map_path;
        rp.replace_extension(".rsm");
        if (std::filesystem::exists(rp)) {
            stamp("phase: parse .rsm");
        }
        if (std::filesystem::exists(rp) && rsm_reader.open(rp.string())) {
            have_rsm = true;
            std::fprintf(stdout, "        .rsm: %zu procs available\n",
                         rsm_reader.procedures().size());
        }
    }

    // Enrich already-emitted globals with their resolved Pascal
    // primitive (kind + width), so the S_GDATA32 records carry
    // proper CodeView types (Int32 / Cardinal / Single / Boolean
    // / WChar / ...) instead of generic void*. Variable resolution
    // is by absolute VA -- reconstruct it from the PublicSymbol's
    // segment + offset coords (mirrors rvaOfPublic going the other
    // way). Only data symbols are affected; functions skip.
    if (have_rsm) {
        std::size_t typed_globals = 0;
        for (auto& ps : inputs.publics) {
            if (ps.is_function) continue;
            if (ps.segment == 0 || ps.segment > inputs.sections.size())
                continue;
            const auto& pe_sec = inputs.sections[ps.segment - 1];
            const std::uint64_t va =
                image_base + pe_sec.virtual_address + ps.offset;
            const auto* v = rsm_reader.findVariableAt(va);
            if (!v || v->pascal_type.empty()) continue;
            if (auto rp = rsm2pdb::rsm::Reader::resolvePrimitive(
                    v->pascal_type)) {
                ps.byte_size = rp->byte_size;
                ps.prim_kind = rp->kind;
                ++typed_globals;
            }
        }
        std::fprintf(stdout,
                     "globals typed via RSM pascal_type: %zu / %zu\n",
                     typed_globals, inputs.publics.size());
    }

    // Per-marker byte-size aggregate from RSM globals; reused by
    // every resolveFunction call below.
    const auto marker_sizes = have_rsm
        ? rsm2pdb::compose::buildMarkerSizes(rsm_reader)
        : std::unordered_map<std::uint8_t, std::uint32_t>{};

    stamp("phase: compose modules");
    // Memoize resolveSourcePath -- on real Delphi projects the
    // same basename ("System.SysUtils.pas") appears in thousands of
    // line tables. Without caching, ~24M filesystem stat calls
    // dominated this phase (5+ minutes on Altium AdvPCB).
    std::unordered_map<std::string, std::string> src_cache;
    auto resolveSourceCached = [&](const std::string& raw) -> std::string {
        auto it = src_cache.find(raw);
        if (it != src_cache.end()) return it->second;
        std::string r = resolveSourcePath(raw, map_path, src_search_dirs);
        src_cache.emplace(raw, r);
        return r;
    };
    // 2b. Compose modules from .map: one DBI module per Pascal
    //     compile unit (= one line_table) carrying its source
    //     path, functions, and line entries. Functions come from
    //     publics whose VA falls in a module_segment for the unit.
    {
        // Index module_segments in segment 1 (.text) by VA range.
        struct UnitRange {
            std::string unit;
            std::uint64_t va_start;
            std::uint64_t va_end;
        };
        std::vector<UnitRange> code_ranges;
        for (const auto& ms : mf.module_segments) {
            if (ms.segment_id != 1) continue;
            const auto* seg = mf.findSegment(ms.segment_id);
            if (!seg) continue;
            UnitRange r;
            r.unit = ms.module_name;
            r.va_start = seg->start_va + ms.segment_offset;
            r.va_end   = r.va_start + ms.length;
            code_ranges.push_back(std::move(r));
        }
        std::sort(code_ranges.begin(), code_ranges.end(),
            [](const UnitRange& a, const UnitRange& b) {
                return a.va_start < b.va_start;
            });
        auto findUnit = [&](std::uint64_t va) -> const UnitRange* {
            auto it = std::upper_bound(
                code_ranges.begin(), code_ranges.end(), va,
                [](std::uint64_t v, const UnitRange& r) {
                    return v < r.va_start;
                });
            if (it == code_ranges.begin()) return nullptr;
            --it;
            if (va < it->va_end) return &*it;
            return nullptr;
        };

        // Collect functions per unit (segment 1 publics only).
        struct FnRaw {
            std::string name;
            std::uint64_t va;
        };
        std::map<std::string, std::vector<FnRaw>> by_unit;
        for (const auto& p : mf.publics) {
            if (p.segment_id != 1) continue;
            const std::uint64_t va = mf.findSegment(p.segment_id)->start_va +
                                     p.segment_offset;
            const auto* ur = findUnit(va);
            if (!ur) continue;
            by_unit[ur->unit].push_back({p.name, va});
        }

        // Build the module list. A single Pascal unit can have
        // multiple line_tables (one per source file pulled in via
        // {$INCLUDE} or named alongside the unit) -- aggregate
        // them by unit name so all line entries make it into the
        // module's C13 subsection.
        std::unordered_map<std::string,
            std::vector<const rsm2pdb::map::LineTable*>> by_name;
        for (const auto& lt : mf.line_tables) {
            by_name[lt.module_name].push_back(&lt);
        }
        std::set<std::string> unit_names;
        for (const auto& kv : by_unit) unit_names.insert(kv.first);
        for (const auto& lt : mf.line_tables) unit_names.insert(lt.module_name);

        std::size_t total_fns = 0, total_lines = 0;
        std::size_t skipped_modules = 0;
        for (const auto& uname : unit_names) {
            if (!include_rtl && isRtlQName(uname)) {
                ++skipped_modules;
                continue;
            }
            rsm2pdb::pdb::Module pdb_mod;
            pdb_mod.name = uname;
            auto it_lts = by_name.find(uname);
            const std::vector<const rsm2pdb::map::LineTable*>* lts =
                it_lts != by_name.end() ? &it_lts->second : nullptr;

            // Sort functions in this unit by VA so we can compute
            // sizes as next-VA gaps.
            auto& raws = by_unit[uname];
            std::sort(raws.begin(), raws.end(),
                [](const FnRaw& a, const FnRaw& b) { return a.va < b.va; });
            for (std::size_t i = 0; i < raws.size(); ++i) {
                const auto& r = raws[i];
                const std::uint64_t rva = r.va - image_base;
                const std::uint16_t seg_idx = findPeSection(rva);
                if (seg_idx == 0) continue;
                const auto& pe_sec = inputs.sections[seg_idx - 1];
                std::uint64_t end_va;
                if (i + 1 < raws.size()) {
                    end_va = raws[i + 1].va;
                } else {
                    const auto* ur = findUnit(r.va);
                    end_va = ur ? ur->va_end : (r.va + 1);
                }
                rsm2pdb::pdb::ModuleFunction mf_out;
                mf_out.name    = r.name;
                mf_out.segment = seg_idx;
                mf_out.offset  = static_cast<std::uint32_t>(
                                    rva - pe_sec.virtual_address);
                mf_out.size    = static_cast<std::uint32_t>(end_va - r.va);

                // Params + locals via compose::resolveFunction --
                // the single source-of-truth Delphi-x64 frame
                // interpretation (sub_rsp / extra_pushes / Self
                // marker / 2-byte-form signed decode / size-
                // resolution chain).
                if (have_rsm) {
                    if (const auto* pr = rsm_reader.findProcedureAt(r.va)) {
                        // Slice of PE bytes the prologue / disasm-
                        // sniffer will read. Empty when the proc's
                        // bytes fall outside the PE; resolveFunction
                        // degrades to sub_rsp=0 in that case.
                        const auto& pe_sec_fn = inputs.sections[
                            mf_out.segment - 1];
                        const std::size_t fn_fo =
                            pe_sec_fn.pointer_to_raw_data
                                + mf_out.offset;
                        const std::uint8_t* code = nullptr;
                        std::size_t code_len = 0;
                        if (fn_fo + mf_out.size <= pe_bytes.size()) {
                            code = pe_bytes.data() + fn_fo;
                            code_len = mf_out.size;
                        }
                        const auto rf =
                            rsm2pdb::compose::resolveFunction(
                                *pr, code, code_len, marker_sizes);
                        for (const auto& rv : rf.vars) {
                            rsm2pdb::pdb::ModuleLocal ml;
                            ml.name      = rv.name;
                            ml.is_param  = rv.is_param;
                            ml.offset    = rv.rbp_offset;
                            ml.byte_size = rv.byte_size;
                            ml.prim_kind = rv.prim_kind;
                            mf_out.locals.push_back(std::move(ml));
                        }
                    }
                }

                pdb_mod.functions.push_back(std::move(mf_out));
            }
            total_fns += pdb_mod.functions.size();

            // Line entries: translate .map LineRecord coords -> PE
            // section-relative (segment, offset). Each .map
            // LineTable becomes one ModuleSource so multi-file
            // units keep all their lines.
            if (lts) {
                for (const auto* lt : *lts) {
                    rsm2pdb::pdb::ModuleSource src;
                    src.source_path = resolveSourceCached(lt->source_path);
                    for (const auto& lr : lt->lines) {
                        const auto* seg = mf.findSegment(lr.segment_id);
                        if (!seg) continue;
                        const std::uint64_t va =
                            seg->start_va + lr.segment_offset;
                        const std::uint64_t rva = va - image_base;
                        const std::uint16_t seg_idx = findPeSection(rva);
                        if (seg_idx == 0) continue;
                        const auto& pe_sec = inputs.sections[seg_idx - 1];
                        rsm2pdb::pdb::ModuleLine ml;
                        ml.segment = seg_idx;
                        ml.offset  = static_cast<std::uint32_t>(
                                        rva - pe_sec.virtual_address);
                        ml.line    = lr.line;
                        src.lines.push_back(ml);
                    }
                    total_lines += src.lines.size();
                    if (!src.lines.empty()) {
                        pdb_mod.sources.push_back(std::move(src));
                    }
                }
            }

            inputs.modules.push_back(std::move(pdb_mod));
        }
        std::fprintf(stdout,
            "modules: %zu (RTL-filtered %zu), S_GPROC32: %zu, line entries: %zu\n",
            inputs.modules.size(), skipped_modules, total_fns, total_lines);
    }

    stamp("phase: write PDB (LLVM streams)");
    // 3. Write the PDB.
    std::string err;
    if (!rsm2pdb::pdb::writePdb(output_pdb, inputs, err)) {
        std::fprintf(stderr, "error: PDB write failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stdout, "wrote PDB: %s\n", output_pdb.c_str());

    stamp("phase: inject RSDS into PE");
    // 4. Inject the matching RSDS pointer into the PE.
    const std::string pdb_basename =
        std::filesystem::path(output_pdb).filename().string();
    if (!rsm2pdb::pe::injectPdbReferenceFile(input_exe, inputs.guid,
                                             inputs.age, pdb_basename,
                                             output_exe, err)) {
        std::fprintf(stderr, "error: PE injection failed: %s\n",
                     err.c_str());
        return 1;
    }
    std::fprintf(stdout, "wrote PE with RSDS pointer: %s\n",
                 output_exe.c_str());
    stamp("done");
    return 0;
}

} // namespace rsm2pdb::cli
