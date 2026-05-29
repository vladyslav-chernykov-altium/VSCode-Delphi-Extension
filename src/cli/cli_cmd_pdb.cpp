#include "cli/cli.h"
#include "cli/source_path.h"
#include "cli/util.h"
#include "compose/frame.h"
#include "map/map_reader.h"
#include "pdb/pdb_writer.h"
#include "pe/pe_pdb_injector.h"
#include "pe/thunk_scanner.h"
#include "rsm/rsm_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
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

    // -- Interface adjuster-thunk detection -----------------------
    //
    // Pattern: `48 83 c1 NN  e9 NN NN NN NN` (9 bytes), Self-adjust
    // + tail-call into the real method. Without an explicit S_PROC32
    // + a C13 line entry covering this PC, cppvsdbg silently demotes
    // step-into-the-thunk to step-over and the user never sees the
    // method body. We resolve each thunk's jmp target against the
    // .map publics; matched thunks get synthesised
    // S_PUB32 + S_GPROC32 entries (named
    // `<TargetMethod>$Adjust_<HexImm>`) and a single line entry so
    // cppvsdbg recognises them as user code. Additionally we
    // generate a sibling `.natstepfilter` listing
    // `.*\$Adjust_[0-9A-F]+$` -- VS2022 / cppvsdbg honour it and
    // tail-call-skip through the thunks on Step Into, landing the
    // user directly in the real method body.
    struct ThunkEmit {
        std::uint64_t va;
        std::uint64_t target_va;
        std::int32_t  adjustment;
        std::uint16_t segment;
        std::uint32_t offset;       // PE-section-relative
        std::string   name;         // synthesised PDB symbol name
        std::string   target_unit;  // .map unit that owns the target
        std::uint32_t target_line;  // first source line of the target
    };
    std::vector<ThunkEmit> thunks_to_emit;
    {
        // Build the code-section descriptors the scanner needs.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> code_secs;
        std::vector<std::pair<std::uint32_t, std::uint16_t>> sec_file;
        for (std::uint16_t i = 0;
             i < inputs.sections.size(); ++i) {
            const auto& s = inputs.sections[i];
            if ((s.characteristics & 0x20u) == 0) continue;  // !CNT_CODE
            code_secs.emplace_back(
                static_cast<std::uint32_t>(s.virtual_address),
                std::max(s.virtual_size, s.size_of_raw_data));
            sec_file.emplace_back(
                static_cast<std::uint32_t>(s.pointer_to_raw_data),
                static_cast<std::uint16_t>(i + 1));
        }
        const auto detected = rsm2pdb::pe::scanAdjusterThunks(
            pe_bytes, code_secs, sec_file, image_base);

        // Index the original .map publics by VA for jmp-target
        // resolution. Use the unfiltered list -- thunks to RTL
        // methods (e.g. System.TInterfacedObject._AddRef) are real
        // thunks and we want to mask them out too via the same
        // .natstepfilter pattern.
        std::unordered_map<std::uint64_t, const rsm2pdb::map::Public*> va_to_pub;
        va_to_pub.reserve(mf.publics.size());
        for (const auto& p : mf.publics) {
            const auto va = (mf.findSegment(p.segment_id)
                             ? mf.findSegment(p.segment_id)->start_va
                             : image_base) + p.segment_offset;
            va_to_pub.emplace(va, &p);
        }

        // .map module_segment + line-table lookup for "which unit
        // does this target VA belong to and what's its first source
        // line" -- the thunk gets attached to that unit's PDB
        // module and points its C13 line entry at the target's
        // first source line.
        //
        // Both helpers used to be linear scans inside the per-thunk
        // loop. On real-world projects (AdvPCB: 947k publics,
        // 142k line-tables, ~5M line records, ~tens-of-thousands of
        // thunks) the inner double-loop in findLineForVa was the
        // hot path -- ~10^11 iterations, several minutes wall-time.
        // Pre-build sorted indexes once and binary-search per
        // thunk: O(log N) per lookup instead of O(N).

        // Unit-by-VA: vector of (va_start, va_end, &unit_name).
        struct UnitSpan {
            std::uint64_t va_start;
            std::uint64_t va_end;
            const std::string* unit;
        };
        std::vector<UnitSpan> unit_spans;
        unit_spans.reserve(mf.module_segments.size());
        for (const auto& ms : mf.module_segments) {
            if (ms.segment_id != 1) continue;
            const auto* seg = mf.findSegment(ms.segment_id);
            if (!seg) continue;
            UnitSpan s;
            s.va_start = seg->start_va + ms.segment_offset;
            s.va_end   = s.va_start + ms.length;
            s.unit     = &ms.module_name;
            unit_spans.push_back(s);
        }
        std::sort(unit_spans.begin(), unit_spans.end(),
            [](const UnitSpan& a, const UnitSpan& b) {
                return a.va_start < b.va_start;
            });
        auto findUnitForVa = [&](std::uint64_t va) -> std::string {
            auto it = std::upper_bound(
                unit_spans.begin(), unit_spans.end(), va,
                [](std::uint64_t v, const UnitSpan& s) {
                    return v < s.va_start;
                });
            if (it == unit_spans.begin()) return {};
            --it;
            return (va < it->va_end) ? *it->unit : std::string{};
        };

        // Line-by-VA: flat vector of (va, line) sorted by va. Each
        // entry collapses any duplicates (same VA, multiple lines)
        // to the smallest line number -- that's the natural "first
        // source line at this PC" semantic.
        std::vector<std::pair<std::uint64_t, std::uint32_t>> line_by_va;
        {
            std::size_t total = 0;
            for (const auto& lt : mf.line_tables) total += lt.lines.size();
            line_by_va.reserve(total);
            for (const auto& lt : mf.line_tables) {
                for (const auto& lr : lt.lines) {
                    const auto* seg = mf.findSegment(lr.segment_id);
                    if (!seg) continue;
                    const auto lva = seg->start_va + lr.segment_offset;
                    line_by_va.emplace_back(lva, lr.line);
                }
            }
            std::sort(line_by_va.begin(), line_by_va.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        auto findLineForVa = [&](std::uint64_t va) -> std::uint32_t {
            if (line_by_va.empty()) return 1;
            auto it = std::upper_bound(
                line_by_va.begin(), line_by_va.end(), va,
                [](std::uint64_t v, const auto& p) { return v < p.first; });
            if (it == line_by_va.begin()) {
                return it->first == va ? it->second : 1;
            }
            --it;
            return it->second;
        };

        std::size_t total_thunks = static_cast<std::size_t>(detected.size());
        std::size_t resolved = 0;
        for (const auto& t : detected) {
            auto it = va_to_pub.find(t.target_va);
            if (it == va_to_pub.end()) continue;     // unresolved jmp
            const auto& tgt = *it->second;
            if (!seg_is_code[tgt.segment_id]) continue;

            // Section coords of the thunk itself.
            const auto& pe_sec_t = inputs.sections[t.segment - 1];
            // sanity guard
            if (t.section_off + rsm2pdb::pe::AdjusterThunk::kSize >
                pe_sec_t.size_of_raw_data) continue;

            ThunkEmit te;
            te.va         = t.va;
            te.target_va  = t.target_va;
            te.adjustment = t.adjustment;
            te.segment    = t.segment;
            te.offset     = t.section_off;
            // Synthesise the name: <target>$Adjust_<HexImm>. HexImm
            // is the 32-bit two's-complement of the adjustment so
            // the value is always positive in the regex
            // .*\$Adjust_[0-9A-F]+$.
            const auto adj_u32 = static_cast<std::uint32_t>(t.adjustment);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "$Adjust_%08X", adj_u32);
            te.name        = tgt.name + buf;
            te.target_unit = findUnitForVa(t.target_va);
            te.target_line = findLineForVa(t.target_va);
            thunks_to_emit.push_back(std::move(te));
            ++resolved;
        }
        std::fprintf(stdout,
            "adjuster thunks: %zu found, %zu resolved to known methods\n",
            total_thunks, resolved);
    }

    // Emit S_PUB32 entries for resolved thunks now so they show up
    // in Call Stack with a meaningful name. Their per-module
    // S_GPROC32 + line entries are added later inside the module
    // composer (we group them by target unit).
    for (const auto& te : thunks_to_emit) {
        rsm2pdb::pdb::PublicSymbol ps;
        ps.name        = te.name;
        ps.segment     = te.segment;
        ps.offset      = te.offset;
        ps.is_function = true;
        inputs.publics.push_back(std::move(ps));
    }

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
    // Aggregate-type registrar (Phase D). Translates an RSM
    // AggregateType into a PdbInputs::aggregates entry on first
    // encounter, returning its index for reuse. Recurses through
    // composite-typed fields so nested types (TBox -> TPoint) get
    // emitted before their containers.
    //
    // For Phase D we ONLY emit records; classes / enums / sets fall
    // back to the existing byte[N] route (Phase E / F will lift them
    // by reusing this same registrar with kind-aware emission).
    std::map<std::pair<std::uint64_t, std::uint16_t>,
             std::size_t> aggr_idx_cache;
    // In-progress set protects against cyclic base-class /
    // composite-field references that we've seen on real-world
    // AdvPCB-scale projects (deep VCL hierarchies + generic
    // container owner-loops). Without this guard the recursive
    // registrar blows the stack -- STATUS_STACK_OVERFLOW observed
    // at registerAggr depth ~thousands on AdvPCB.
    std::set<std::pair<std::uint64_t, std::uint16_t>> aggr_in_progress;
    // Self placeholders for opaque / forward-declared classes whose
    // 0x2a record doesn't carry fields (e.g. AdvPCB's TPCBCommands).
    // Keyed by class name so cdb sees a single typed pointer per
    // class instead of one synthetic LF_CLASS per method.
    std::map<std::string, std::size_t> self_opaque_cache;
    // Anonymous in-proc set placeholders. Keyed by (unit_anchor,
    // base_enum_own_hash) so multiple `r: set of TXxx` locals
    // sharing the same enum get one LF_BITFIELD struct.
    std::map<std::pair<std::uint64_t, std::uint16_t>,
             std::size_t> inproc_set_cache;
    // Pre-built indexes over rsm_reader.aggregates() so the per-Self
    // class-name lookup runs in O(1) instead of O(N) per call.
    // Without these, an AdvPCB-scale build (~3k class methods,
    // ~66k aggregates) spends 100+ seconds linearly scanning the
    // aggregate list for every Self.
    std::unordered_multimap<std::string,
                            const rsm2pdb::rsm::AggregateType*>
        agg_by_name;
    // Same-unit lookup wins when both kinds match; key on
    // (unit_anchor, name) to avoid cross-unit confusion.
    struct UA_Name {
        std::uint64_t ua;
        std::string  name;
        bool operator==(const UA_Name& o) const {
            return ua == o.ua && name == o.name;
        }
    };
    struct UA_Name_Hash {
        std::size_t operator()(const UA_Name& k) const noexcept {
            return std::hash<std::uint64_t>{}(k.ua) * 1099511628211ull
                 ^ std::hash<std::string>{}(k.name);
        }
    };
    std::unordered_multimap<UA_Name,
                            const rsm2pdb::rsm::AggregateType*,
                            UA_Name_Hash>
        agg_by_unit_name;
    if (have_rsm) {
        agg_by_name.reserve(rsm_reader.aggregates().size());
        agg_by_unit_name.reserve(rsm_reader.aggregates().size());
        for (const auto& a : rsm_reader.aggregates()) {
            agg_by_name.emplace(a.name, &a);
            agg_by_unit_name.emplace(
                UA_Name{a.unit_anchor_offset, a.name}, &a);
        }
    }
    std::function<std::optional<std::size_t>(
        const rsm2pdb::rsm::AggregateType*)> registerAggr;
    registerAggr = [&](const rsm2pdb::rsm::AggregateType* a)
            -> std::optional<std::size_t> {
        if (a == nullptr) return std::nullopt;
        const bool is_record =
               a->kind == rsm2pdb::rsm::AggregateKind::Record
            || a->kind == rsm2pdb::rsm::AggregateKind::PackedRecord;
        const bool is_class =
               a->kind == rsm2pdb::rsm::AggregateKind::Class;
        const bool is_enum =
               a->kind == rsm2pdb::rsm::AggregateKind::Enum;

        // Set detection -- RSM doesn't carry an explicit "set of X"
        // marker on the type record, so we infer from a name-suffix
        // heuristic: an aggregate with kind Unknown (no fields, no
        // enum entries, no class header) whose name is a registered
        // Enum's name plus a trailing `s`. Hits TColors -> TColor,
        // TKeys -> TKey, etc. False positives are rare because the
        // base-enum lookup must succeed in the SAME unit.
        const rsm2pdb::rsm::AggregateType* set_base = nullptr;
        if (a->kind == rsm2pdb::rsm::AggregateKind::Unknown
            && a->name.size() > 1
            && a->name.back() == 's') {
            const std::string base_name(a->name.data(), a->name.size() - 1);
            for (const auto& cand : rsm_reader.aggregates()) {
                if (cand.unit_anchor_offset != a->unit_anchor_offset) continue;
                if (cand.kind != rsm2pdb::rsm::AggregateKind::Enum) continue;
                if (cand.name != base_name) continue;
                set_base = &cand;
                break;
            }
        }
        const bool is_set = (set_base != nullptr);

        if (!is_record && !is_class && !is_enum && !is_set) {
            // Sets we couldn't pin down + Unknowns -- still on the
            // byte[N] fallback.
            return std::nullopt;
        }
        const std::pair<std::uint64_t, std::uint16_t> key{
            a->unit_anchor_offset, a->own_hash};
        if (auto it = aggr_idx_cache.find(key);
            it != aggr_idx_cache.end()) {
            return it->second;
        }
        // Cycle break: if we're already registering this aggregate
        // (deeper in the call stack), bail out -- treat the
        // recursive reference as "unresolved". The writer's
        // dependency-order invariant (children emitted before
        // referrers) requires that registerAggr push to
        // inputs.aggregates AFTER recursing on children, so we
        // can't reserve a slot upfront without breaking that
        // invariant -- this is the cheapest workable alternative.
        if (!aggr_in_progress.insert(key).second) {
            return std::nullopt;
        }
        // For classes, recurse on the base class FIRST so its
        // index is registered before our own. The recursion may
        // return nullopt if it hits a cycle -- we treat that as
        // "no explicit base" (CodeView LF_BCLASS chains don't
        // cycle anyway).
        std::optional<std::size_t> base_idx;
        if (is_class && a->base_hash != 0) {
            const rsm2pdb::rsm::AggregateType* base_a =
                rsm_reader.findAggregateInUnit(a->unit_anchor_offset,
                                               a->base_hash);
            if (base_a == nullptr) {
                base_a = rsm_reader.findAggregateByHash(a->base_hash);
            }
            base_idx = registerAggr(base_a);
        }
        const auto idx = inputs.aggregates.size();
        aggr_idx_cache[key] = idx;
        rsm2pdb::pdb::AggregateRecord rec;
        rec.kind      = is_class
                        ? rsm2pdb::pdb::AggregateKind::Class
                        : is_enum
                            ? rsm2pdb::pdb::AggregateKind::Enum
                            : is_set
                                ? rsm2pdb::pdb::AggregateKind::Set
                                : rsm2pdb::pdb::AggregateKind::Record;
        rec.name      = a->name;
        rec.byte_size = a->total_size;
        rec.base      = base_idx;
        if (is_set) {
            // Borrow the base enum's enumerator names (one bit each).
            // Set width: Delphi rounds to the smallest power-of-two
            // byte count that fits all enumerators; floor at 1 byte.
            // For 4-value TColor we get 1 byte; for 33+ enumerators
            // it'd be 8 bytes. AdvPCB rarely needs >4.
            rec.enumerators.reserve(set_base->enum_entries.size());
            std::int64_t max_ord = 0;
            for (const auto& e : set_base->enum_entries) {
                rsm2pdb::pdb::AggregateEnumerator ae;
                ae.name  = e.name;
                ae.value = e.ordinal;
                rec.enumerators.push_back(std::move(ae));
                if (e.ordinal > max_ord) max_ord = e.ordinal;
            }
            rec.byte_size =
                  max_ord <= 7   ? 1
                : max_ord <= 15  ? 2
                : max_ord <= 31  ? 4
                                 : 8;
            inputs.aggregates.push_back(std::move(rec));
            aggr_in_progress.erase(key);
            return idx;
        }
        if (is_enum) {
            // Enum size follows Delphi's default rule: max ordinal
            // <= 0xFF -> 1 byte, <= 0xFFFF -> 2 bytes, else 4.
            // {$MINENUMSIZE} can override; we don't model it yet.
            std::int64_t max_ord = 0;
            for (const auto& e : a->enum_entries) {
                if (e.ordinal > max_ord) max_ord = e.ordinal;
            }
            rec.byte_size = max_ord <= 0xFF      ? 1
                          : max_ord <= 0xFFFF    ? 2
                                                 : 4;
            rec.enumerators.reserve(a->enum_entries.size());
            for (const auto& e : a->enum_entries) {
                rsm2pdb::pdb::AggregateEnumerator ae;
                ae.name  = e.name;
                ae.value = e.ordinal;
                rec.enumerators.push_back(std::move(ae));
            }
            inputs.aggregates.push_back(std::move(rec));
            aggr_in_progress.erase(key);
            return idx;
        }
        rec.fields.reserve(a->fields.size());
        for (const auto& fe : a->fields) {
            rsm2pdb::pdb::AggregateField f;
            f.name        = fe.name;
            f.byte_offset = fe.offset;
            if (fe.type_hash != 0) {
                // Composite field. Same-unit first, then last-wins.
                const rsm2pdb::rsm::AggregateType* sub =
                    rsm_reader.findAggregateInUnit(
                        a->unit_anchor_offset, fe.type_hash);
                if (sub == nullptr) {
                    sub = rsm_reader.findAggregateByHash(fe.type_hash);
                }
                f.nested_aggregate = registerAggr(sub);
            } else if (fe.primitive_marker != 0) {
                if (const auto* nm = rsm_reader.primitiveNameForMarker(
                        a->unit_anchor_offset, fe.primitive_marker)) {
                    if (auto rp = rsm2pdb::rsm::Reader::resolvePrimitive(
                            *nm)) {
                        f.prim_kind = rp->kind;
                        f.byte_size = rp->byte_size;
                    }
                }
                // f.prim_kind stays nullopt when the marker's name
                // isn't a known primitive (e.g. ShortString or an
                // RTL-only type we don't decode) -- writer falls
                // back to UChar.
            }
            rec.fields.push_back(std::move(f));
        }
        inputs.aggregates.push_back(std::move(rec));
        aggr_in_progress.erase(key);
        return idx;
    };
    auto lookupAggrIdx = [&](const rsm2pdb::rsm::Variable& v)
            -> std::optional<std::size_t> {
        if (v.is_primitive || v.inline_type_id == 0) return std::nullopt;
        const rsm2pdb::rsm::AggregateType* a = nullptr;
        if (v.unit_anchor_offset != 0) {
            a = rsm_reader.findAggregateInUnit(
                v.unit_anchor_offset, v.inline_type_id);
        }
        if (a == nullptr) {
            a = rsm_reader.findAggregateByHash(v.inline_type_id);
        }
        return registerAggr(a);
    };

    if (have_rsm) {
        std::size_t typed_globals  = 0;
        std::size_t aggr_globals   = 0;
        for (auto& ps : inputs.publics) {
            if (ps.is_function) continue;
            if (ps.segment == 0 || ps.segment > inputs.sections.size())
                continue;
            const auto& pe_sec = inputs.sections[ps.segment - 1];
            const std::uint64_t va =
                image_base + pe_sec.virtual_address + ps.offset;
            const auto* v = rsm_reader.findVariableAt(va);
            if (!v) continue;
            // Primitive globals: resolve via Pascal-name -> primitive.
            if (!v->pascal_type.empty()) {
                if (auto rp = rsm2pdb::rsm::Reader::resolvePrimitive(
                        v->pascal_type)) {
                    ps.byte_size = rp->byte_size;
                    ps.prim_kind = rp->kind;
                    ++typed_globals;
                    continue;
                }
            }
            // Non-primitive globals: route through aggregate registrar.
            // Records get LF_STRUCTURE in the PDB; classes / enums
            // stay on the byte[N] fallback for now.
            if (auto idx = lookupAggrIdx(*v)) {
                ps.aggregate_index = idx;
                ps.byte_size       =
                    inputs.aggregates[*idx].byte_size != 0
                        ? inputs.aggregates[*idx].byte_size
                        : ps.byte_size;
                ++aggr_globals;
            }
        }
        std::fprintf(stdout,
                     "globals typed via RSM pascal_type: %zu / %zu "
                     "(records routed via TPI: %zu, %zu aggregates "
                     "registered)\n",
                     typed_globals, inputs.publics.size(),
                     aggr_globals, inputs.aggregates.size());
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
                            // Phase D: route record-typed locals
                            // through the aggregate registrar so they
                            // emit LF_STRUCTURE-backed TypeIndex
                            // instead of byte[N]. compose stamps
                            // aggregate_hash + unit_anchor_offset on
                            // the resolved var; we look the aggregate
                            // up and register it (no-op if already
                            // seen).
                            if (rv.aggregate_hash != 0) {
                                const rsm2pdb::rsm::AggregateType* a =
                                    rsm_reader.findAggregateInUnit(
                                        rv.unit_anchor_offset,
                                        rv.aggregate_hash);
                                if (a == nullptr) {
                                    a = rsm_reader.findAggregateByHash(
                                        rv.aggregate_hash);
                                }
                                if (auto idx = registerAggr(a)) {
                                    ml.aggregate_index = idx;
                                    if (inputs.aggregates[*idx].byte_size)
                                        ml.byte_size =
                                            inputs.aggregates[*idx].byte_size;
                                }
                            }
                            // Anonymous in-proc `set of TXxx` local.
                            // Delphi declares the set's type
                            // implicitly (no 0x2a record), so r's
                            // type_hash won't resolve. The companion
                            // 0x2a record for the BASE ENUM sits a
                            // few hashes earlier in the same unit;
                            // empirically the set's hash =
                            // enum_hash + 4. If we find one within
                            // a small window, synthesise an
                            // LF_BITFIELD-struct view using the
                            // enum's enumerators -- cdb / VS show
                            // which bits are set instead of an
                            // opaque void*.
                            if (rv.aggregate_hash != 0
                                && !ml.aggregate_index
                                && !rv.is_self) {
                                const rsm2pdb::rsm::AggregateType*
                                    enum_a = nullptr;
                                int best_diff = 16;
                                for (const auto& a :
                                     rsm_reader.aggregates()) {
                                    if (a.kind != rsm2pdb::rsm
                                            ::AggregateKind::Enum)
                                        continue;
                                    if (a.unit_anchor_offset
                                        != rv.unit_anchor_offset)
                                        continue;
                                    const int diff =
                                        static_cast<int>(
                                            rv.aggregate_hash)
                                        - static_cast<int>(a.own_hash);
                                    if (diff >= 1 && diff <= best_diff) {
                                        best_diff = diff;
                                        enum_a    = &a;
                                    }
                                }
                                if (enum_a != nullptr) {
                                    const auto key = std::make_pair(
                                        rv.unit_anchor_offset,
                                        enum_a->own_hash);
                                    std::size_t set_idx;
                                    auto it = inproc_set_cache.find(key);
                                    if (it != inproc_set_cache.end()) {
                                        set_idx = it->second;
                                    } else {
                                        rsm2pdb::pdb::AggregateRecord rec;
                                        rec.kind = rsm2pdb::pdb
                                            ::AggregateKind::Set;
                                        rec.name = "set of "
                                                 + enum_a->name;
                                        std::int64_t max_ord = 0;
                                        rec.enumerators.reserve(
                                            enum_a->enum_entries.size());
                                        for (const auto& e :
                                             enum_a->enum_entries) {
                                            rsm2pdb::pdb
                                                ::AggregateEnumerator ae;
                                            ae.name  = e.name;
                                            ae.value = e.ordinal;
                                            rec.enumerators
                                                .push_back(std::move(ae));
                                            if (e.ordinal > max_ord)
                                                max_ord = e.ordinal;
                                        }
                                        rec.byte_size =
                                              max_ord <= 7   ? 1
                                            : max_ord <= 15  ? 2
                                            : max_ord <= 31  ? 4
                                                             : 8;
                                        set_idx =
                                            inputs.aggregates.size();
                                        inputs.aggregates.push_back(
                                            std::move(rec));
                                        inproc_set_cache[key] = set_idx;
                                    }
                                    ml.aggregate_index = set_idx;
                                    ml.byte_size =
                                        inputs.aggregates[set_idx]
                                            .byte_size;
                                }
                            }
                            // Self of a class method: ALWAYS derive
                            // type from the enclosing class via the
                            // proc-name suffix instead of trusting
                            // RSM's Self encoding (which can carry
                            // sentinel markers like 0x29 / 0xD5 that
                            // don't resolve to an aggregate, or a
                            // 2-byte encoded marker like 0x0405 that
                            // matches nothing).
                            //
                            // Pascal `procedure TClass.Method` -> the
                            // implicit Self has type `TClass*`. Strip
                            // the trailing `.Method`, then strip the
                            // leading unit prefix. Look the class up:
                            //   1. Same unit, kind = Class    (best)
                            //   2. Any unit,  kind = Class    (class
                            //      declared in interface-only unit)
                            //   3. Any unit,  any kind        (opaque
                            //      / forward-declared classes, AdvPCB
                            //      style -- TPCBCommands has a 0x2a
                            //      record but no class header or
                            //      fields). registerAsClass synthesises
                            //      an empty LF_CLASS so cdb at least
                            //      knows it's a typed pointer.
                            //   4. Nothing found              -> emit
                            //      Self as void* (typeForSize(0)
                            //      yields VoidPointer64, the only
                            //      pointer-ish primitive we have).
                            if (rv.is_self) {
                                const auto& pn = pr->name;
                                const auto last_dot = pn.rfind('.');
                                std::string class_name;
                                if (last_dot != std::string::npos
                                    && last_dot > 0) {
                                    const auto prev_dot =
                                        pn.rfind('.', last_dot - 1);
                                    class_name =
                                        (prev_dot == std::string::npos)
                                            ? pn.substr(0, last_dot)
                                            : pn.substr(prev_dot + 1,
                                                last_dot - prev_dot - 1);
                                }
                                if (!class_name.empty()) {
                                    const auto ua =
                                        rsm_reader.unitAnchorFor(
                                            pr->file_offset);
                                    const rsm2pdb::rsm::AggregateType*
                                        cls = nullptr;
                                    // Pass 1: same unit, kind=Class.
                                    auto r1 = agg_by_unit_name.equal_range(
                                        UA_Name{ua, class_name});
                                    for (auto it = r1.first;
                                         it != r1.second && !cls; ++it) {
                                        if (it->second->kind
                                            == rsm2pdb::rsm
                                                  ::AggregateKind::Class) {
                                            cls = it->second;
                                        }
                                    }
                                    // Pass 2: any unit, kind=Class.
                                    if (cls == nullptr) {
                                        auto r2 =
                                            agg_by_name.equal_range(
                                                class_name);
                                        for (auto it = r2.first;
                                             it != r2.second && !cls;
                                             ++it) {
                                            if (it->second->kind
                                                == rsm2pdb::rsm
                                                      ::AggregateKind
                                                          ::Class) {
                                                cls = it->second;
                                            }
                                        }
                                    }
                                    // Pass 3: any kind, any unit
                                    // (TPCBCommands-style opaque).
                                    if (cls == nullptr) {
                                        auto r3 =
                                            agg_by_name.equal_range(
                                                class_name);
                                        if (r3.first != r3.second) {
                                            cls = r3.first->second;
                                        }
                                    }
                                    std::optional<std::size_t> self_idx;
                                    if (cls != nullptr
                                        && cls->kind == rsm2pdb::rsm
                                              ::AggregateKind::Class) {
                                        // Full class -- normal path.
                                        self_idx = registerAggr(cls);
                                    } else {
                                        // Opaque / not found:
                                        // synthesise an empty
                                        // LF_CLASS placeholder so
                                        // Self at least shows up as
                                        // `TClassName *` with no
                                        // fields. Cached by name.
                                        auto cit = self_opaque_cache
                                                       .find(class_name);
                                        if (cit != self_opaque_cache.end()) {
                                            self_idx = cit->second;
                                        } else {
                                            const auto idx =
                                                inputs.aggregates.size();
                                            rsm2pdb::pdb::AggregateRecord
                                                rec;
                                            rec.kind = rsm2pdb::pdb
                                                ::AggregateKind::Class;
                                            rec.name = class_name;
                                            rec.byte_size = 0;
                                            inputs.aggregates.push_back(
                                                std::move(rec));
                                            self_opaque_cache[class_name] =
                                                idx;
                                            self_idx = idx;
                                        }
                                    }
                                    if (self_idx) {
                                        ml.aggregate_index = self_idx;
                                        ml.byte_size = 8;
                                    } else {
                                        // No class info AT ALL --
                                        // void* hint is still better
                                        // than unsigned __int64.
                                        ml.byte_size = 0;
                                        ml.prim_kind = std::nullopt;
                                    }
                                }
                            }
                            mf_out.locals.push_back(std::move(ml));
                        }
                    }
                }

                pdb_mod.functions.push_back(std::move(mf_out));
            }

            // Inject the adjuster thunks belonging to this unit.
            // Each gets a tiny S_GPROC32 entry (size = 9 bytes,
            // matching the `add rcx, imm8; jmp rel32` shape) so
            // cppvsdbg recognises the PC as user code and steps
            // into it; without this the engine demotes Step Into
            // to Step Over on the indirect call that targets the
            // thunk. The single line entry below points at the
            // target's first source line -- the user briefly sees
            // that source location during the descent through the
            // thunk, then lands in the real method.
            for (const auto& te : thunks_to_emit) {
                if (te.target_unit != uname) continue;
                rsm2pdb::pdb::ModuleFunction tf;
                tf.name    = te.name;
                tf.segment = te.segment;
                tf.offset  = te.offset;
                tf.size    = rsm2pdb::pe::AdjusterThunk::kSize;
                pdb_mod.functions.push_back(std::move(tf));
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
                    // Append thunk line entries to the FIRST source
                    // of this unit (whatever the .map's primary
                    // line-table is). The single per-thunk line
                    // entry covers the thunk's PC range (9 bytes)
                    // and points at the target method's first
                    // source line -- enough for cppvsdbg to treat
                    // the thunk as user code instead of degrading
                    // Step Into to Step Over.
                    if (lt == (*lts)[0]) {
                        for (const auto& te : thunks_to_emit) {
                            if (te.target_unit != uname) continue;
                            rsm2pdb::pdb::ModuleLine ml;
                            ml.segment = te.segment;
                            ml.offset  = te.offset;
                            ml.line    = te.target_line;
                            src.lines.push_back(ml);
                        }
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

    // Emit the sibling .natstepfilter alongside the PDB so VS2022's
    // native debugger (and cppvsdbg / WinDbg via the same machinery)
    // tail-call-skips through every `<Method>$Adjust_<HexImm>`
    // adjuster thunk on Step Into. Without this VS would single-
    // step the thunk's `add rcx, ...` + `jmp` before landing in the
    // real method; with it, F11 from an interface call lands the
    // user directly in the method body.
    //
    // Only emit when at least one thunk was detected -- otherwise
    // a stale empty .natstepfilter near a PDB is just noise.
    if (!thunks_to_emit.empty()) {
        const std::filesystem::path nspath =
            std::filesystem::path(output_pdb).replace_extension(".natstepfilter");
        std::ofstream f(nspath, std::ios::binary | std::ios::trunc);
        if (f) {
            f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
              << "<StepFilter xmlns=\"http://schemas.microsoft.com/"
                 "vstudio/debugger/natstepfilter/2010\">\n"
              << "    <Function>\n"
              << "        <Name>.*\\$Adjust_[0-9A-Fa-f]+$</Name>\n"
              << "        <Action>NoStepInto</Action>\n"
              << "    </Function>\n"
              << "</StepFilter>\n";
            std::fprintf(stdout,
                "wrote natstepfilter: %s (%zu adjuster thunks)\n",
                nspath.string().c_str(), thunks_to_emit.size());
        } else {
            std::fprintf(stderr,
                "warning: couldn't write natstepfilter to %s\n",
                nspath.string().c_str());
        }
    }

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
