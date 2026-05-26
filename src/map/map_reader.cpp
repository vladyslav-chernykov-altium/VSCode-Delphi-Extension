#include "map/map_reader.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rsm2pdb::map {

// -------------------------------------------------------------------------
// Public helpers
// -------------------------------------------------------------------------

const Segment* MapFile::findSegment(std::uint16_t id) const {
    for (const auto& s : segments) if (s.id == id) return &s;
    return nullptr;
}

// -------------------------------------------------------------------------
// Reader
// -------------------------------------------------------------------------

bool Reader::open(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error_ = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse(ss.str());
}

// -------------------------------------------------------------------------
// Tokenization helpers (kept tiny - the format is regular enough)
// -------------------------------------------------------------------------

namespace {

std::string_view rstrip(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' ||
                          s.back() == '\t' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string_view lstrip(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    return s;
}

std::string_view strip(std::string_view s) { return lstrip(rstrip(s)); }

// Parse "AAAA:BBBBBBBB" -> (AAAA, BBBBBBBB). Returns true on success.
bool parseSegOffset(std::string_view tok,
                    std::uint16_t& segment,
                    std::uint64_t& offset) {
    auto colon = tok.find(':');
    if (colon == std::string_view::npos) return false;
    auto seg_s = tok.substr(0, colon);
    auto off_s = tok.substr(colon + 1);
    char* end = nullptr;
    segment = static_cast<std::uint16_t>(std::strtoul(
        std::string(seg_s).c_str(), &end, 16));
    if (!end || *end != 0) return false;
    offset = std::strtoull(std::string(off_s).c_str(), &end, 16);
    if (!end || *end != 0) return false;
    return true;
}

// Trailing 'H' is sometimes appended to hex numbers in segment overview.
std::uint64_t parseHex(std::string_view tok) {
    if (!tok.empty() && (tok.back() == 'H' || tok.back() == 'h')) {
        tok.remove_suffix(1);
    }
    return std::strtoull(std::string(tok).c_str(), nullptr, 16);
}

// Split a line into whitespace-separated tokens.
std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        if (i == line.size()) break;
        std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        out.emplace_back(line.substr(start, i - start));
    }
    return out;
}

} // namespace

// -------------------------------------------------------------------------
// Parser
// -------------------------------------------------------------------------

bool Reader::parse(const std::string& text) {
    enum class State {
        Initial,
        Segments,       // after "Start         Length     Name ..."
        DetailedMap,    // after "Detailed map of segments"
        PublicsByName,  // after "Address             Publics by Name"
        PublicsByValue, // after "Address             Publics by Value" - skipped
        LineNumbers,    // after "Line numbers for X(file.dpr) segment .text"
        Resources,      // after "Bound resource files" - ignored
        EntryPoint,     // after seeing "Program entry point at SEG:OFF"
        Done
    };

    State state = State::Initial;
    LineTable current_table{};  // accumulator when in LineNumbers

    // Section header detection (line-by-line)
    static const std::regex re_segments_hdr(
        R"(^\s*Start\s+Length\s+Name\s+Class\s*$)");
    static const std::regex re_detailed_hdr(
        R"(^\s*Detailed map of segments\s*$)");
    static const std::regex re_pub_name_hdr(
        R"(^\s*Address\s+Publics by Name\s*$)");
    static const std::regex re_pub_value_hdr(
        R"(^\s*Address\s+Publics by Value\s*$)");
    static const std::regex re_line_hdr(
        R"(^\s*Line numbers for (\S+)\((.+)\)\s+segment\s+(\S+)\s*$)");
    static const std::regex re_resource_hdr(
        R"(^\s*Bound resource files\s*$)");
    static const std::regex re_entry_point(
        R"(^\s*Program entry point at\s+([0-9a-fA-F]+):([0-9a-fA-F]+)\s*$)");

    auto flushLineTable = [&]() {
        if (!current_table.lines.empty()) {
            file_.line_tables.push_back(std::move(current_table));
        }
        current_table = LineTable{};
    };

    std::istringstream in(text);
    std::string raw;
    std::size_t line_no = 0;

    while (std::getline(in, raw)) {
        ++line_no;
        std::string_view line = raw;

        // Section header transitions first (high priority).
        std::smatch m;
        if (std::regex_match(raw, m, re_segments_hdr)) {
            flushLineTable();
            state = State::Segments;
            continue;
        }
        if (std::regex_match(raw, m, re_detailed_hdr)) {
            flushLineTable();
            state = State::DetailedMap;
            continue;
        }
        if (std::regex_match(raw, m, re_pub_name_hdr)) {
            flushLineTable();
            state = State::PublicsByName;
            continue;
        }
        if (std::regex_match(raw, m, re_pub_value_hdr)) {
            flushLineTable();
            state = State::PublicsByValue;
            continue;
        }
        if (std::regex_match(raw, m, re_resource_hdr)) {
            flushLineTable();
            state = State::Resources;
            continue;
        }
        if (std::regex_match(raw, m, re_line_hdr)) {
            flushLineTable();
            current_table.module_name = m[1].str();
            current_table.source_path = m[2].str();
            current_table.segment_name = m[3].str();
            state = State::LineNumbers;
            continue;
        }
        if (std::regex_match(raw, m, re_entry_point)) {
            file_.entry_point.has_value = true;
            file_.entry_point.segment_id = static_cast<std::uint16_t>(
                std::strtoul(m[1].str().c_str(), nullptr, 16));
            file_.entry_point.segment_offset = std::strtoull(
                m[2].str().c_str(), nullptr, 16);
            state = State::EntryPoint;
            continue;
        }

        // Skip blank lines.
        if (strip(line).empty()) continue;

        // Row dispatch by state.
        switch (state) {
        case State::Segments: {
            // " 0001:00401000 00029194H .text                   CODE"
            auto toks = tokenize(line);
            if (toks.size() < 4) break;
            Segment seg{};
            if (!parseSegOffset(toks[0], seg.id, seg.start_va)) break;
            seg.length = parseHex(toks[1]);
            seg.name   = std::string(toks[2]);
            seg.klass  = std::string(toks[3]);
            file_.segments.push_back(std::move(seg));
            break;
        }

        case State::DetailedMap: {
            // " 0001:00000000 000100BC C=CODE     S=.text    G=(none)   M=System   ALIGN=4"
            auto toks = tokenize(line);
            if (toks.size() < 6) break;
            ModuleInSegment mis{};
            if (!parseSegOffset(toks[0], mis.segment_id, mis.segment_offset)) break;
            mis.length = parseHex(toks[1]);
            for (std::size_t i = 2; i < toks.size(); ++i) {
                auto t = toks[i];
                auto eq = t.find('=');
                if (eq == std::string_view::npos) continue;
                auto k = t.substr(0, eq);
                auto v = t.substr(eq + 1);
                if (k == "C")          mis.klass     = std::string(v);
                else if (k == "S")     mis.section   = std::string(v);
                else if (k == "G")     mis.group     = std::string(v);
                else if (k == "M")     mis.module_name = std::string(v);
                else if (k == "ALIGN") mis.alignment = static_cast<std::uint32_t>(
                                            std::strtoul(std::string(v).c_str(),
                                                         nullptr, 10));
            }
            file_.module_segments.push_back(std::move(mis));
            break;
        }

        case State::PublicsByName: {
            // " 0001:00025900       hello.Add"
            auto toks = tokenize(line);
            if (toks.size() < 2) break;
            Public p{};
            if (!parseSegOffset(toks[0], p.segment_id, p.segment_offset)) break;
            // The name can contain spaces? Delphi usually doesn't but
            // the map row in our fixture never does. Use the rest joined.
            std::string name(toks[1]);
            for (std::size_t i = 2; i < toks.size(); ++i) {
                name += ' ';
                name += std::string(toks[i]);
            }
            p.name = std::move(name);
            file_.publics.push_back(std::move(p));
            break;
        }

        case State::PublicsByValue:
        case State::Resources:
            // Same data we already have, or content we ignore.
            break;

        case State::LineNumbers: {
            // Rows look like:
            //   "    24 0001:00025900    25 0001:0002590E    26 0001:00025917    27 0001:0002591D"
            // Multiple (line, seg:off) tuples per row.
            auto toks = tokenize(line);
            for (std::size_t i = 0; i + 1 < toks.size(); i += 2) {
                LineRecord lr{};
                lr.line = static_cast<std::uint32_t>(std::strtoul(
                    std::string(toks[i]).c_str(), nullptr, 10));
                if (!parseSegOffset(toks[i + 1], lr.segment_id, lr.segment_offset)) {
                    // bad row - skip
                    continue;
                }
                current_table.lines.push_back(std::move(lr));
            }
            break;
        }

        case State::Initial:
        case State::EntryPoint:
        case State::Done:
            break;
        }
    }

    // Flush any trailing line table.
    flushLineTable();

    return true;
}

// -------------------------------------------------------------------------
// Dump
// -------------------------------------------------------------------------

void Reader::dump(std::FILE* out) const {
    std::fprintf(out, "=== Segments (%zu) ===\n", file_.segments.size());
    for (const auto& s : file_.segments) {
        std::fprintf(out, "  %04x  va=0x%016llx  len=0x%llx  %-10s class=%s\n",
                     (unsigned)s.id, (unsigned long long)s.start_va,
                     (unsigned long long)s.length,
                     s.name.c_str(), s.klass.c_str());
    }

    std::fprintf(out, "\n=== Module segments (%zu) ===\n",
                 file_.module_segments.size());
    for (const auto& m : file_.module_segments) {
        std::fprintf(out,
            "  %04x:%08llx  len=0x%llx  S=%-10s M=%-30s C=%-6s ALIGN=%u\n",
            (unsigned)m.segment_id, (unsigned long long)m.segment_offset,
            (unsigned long long)m.length, m.section.c_str(),
            m.module_name.c_str(), m.klass.c_str(), (unsigned)m.alignment);
    }

    std::fprintf(out, "\n=== Publics (%zu) ===\n", file_.publics.size());
    // Limit to first 20 for sanity; full dump is large.
    std::size_t shown = 0;
    for (const auto& p : file_.publics) {
        if (shown >= 20) break;
        std::fprintf(out, "  %04x:%08llx  %s\n",
                     (unsigned)p.segment_id,
                     (unsigned long long)p.segment_offset,
                     p.name.c_str());
        ++shown;
    }
    if (file_.publics.size() > 20) {
        std::fprintf(out, "  ... and %zu more\n", file_.publics.size() - 20);
    }

    std::fprintf(out, "\n=== Line tables (%zu) ===\n", file_.line_tables.size());
    for (const auto& lt : file_.line_tables) {
        std::fprintf(out, "  %s (%s) segment %s : %zu entries\n",
                     lt.module_name.c_str(), lt.source_path.c_str(),
                     lt.segment_name.c_str(), lt.lines.size());
        std::size_t shown_l = 0;
        for (const auto& l : lt.lines) {
            if (shown_l >= 5) {
                std::fprintf(out, "    ... and %zu more\n",
                             lt.lines.size() - shown_l);
                break;
            }
            std::fprintf(out, "    line %u  at %04x:%08llx\n",
                         l.line, (unsigned)l.segment_id,
                         (unsigned long long)l.segment_offset);
            ++shown_l;
        }
    }

    if (file_.entry_point.has_value) {
        std::fprintf(out, "\n=== Entry point: %04x:%08llx ===\n",
                     (unsigned)file_.entry_point.segment_id,
                     (unsigned long long)file_.entry_point.segment_offset);
    }
}

// -------------------------------------------------------------------------
// Adapter: MapFile -> model::Module
// -------------------------------------------------------------------------

namespace {

// Compute absolute virtual address for a (segment_id, segment_offset)
// pair by looking up the segment's start_va.
std::uint64_t resolveVA(const MapFile& mf,
                        std::uint16_t segment_id,
                        std::uint64_t segment_offset) {
    if (const auto* seg = mf.findSegment(segment_id)) {
        return seg->start_va + segment_offset;
    }
    return 0;
}

} // namespace

void populate(const MapFile& mf, model::Module& mod) {
    // Source files: one per unique line-table source_path.
    // Returned source_files indices match LineEntry.file_id.
    auto sourceFileId = [&](const std::string& path) -> std::uint32_t {
        for (std::size_t i = 0; i < mod.source_files.size(); ++i) {
            if (mod.source_files[i] == path) {
                return static_cast<std::uint32_t>(i);
            }
        }
        mod.source_files.push_back(path);
        return static_cast<std::uint32_t>(mod.source_files.size() - 1);
    };

    // Assign each public to EXACTLY ONE CompileUnit by finding the
    // LONGEST registered-unit-name prefix in its dotted Pascal name.
    //
    // Without dedup, a public like "App.Colors.ColorName" gets added
    // to BOTH a (hypothetical) "App" unit AND the "App.Colors" unit,
    // blowing up the model. Real-world example: Altium's 946k-public
    // .map produced 54 MILLION symbols (~57x dup) with the naive
    // matching.
    //
    // Algorithm:
    //   pass 1: build module-name set from line_tables; bucket publics
    //           by their longest-matching module prefix.
    //   pass 2: iterate line tables, pull just their bucket.
    //
    // Cost: O(publics * max_dots) which is essentially O(publics).
    std::unordered_set<std::string> module_names;
    module_names.reserve(mf.line_tables.size() * 2);
    for (const auto& lt : mf.line_tables) module_names.insert(lt.module_name);

    std::unordered_map<std::string, std::vector<std::size_t>> publics_by_module;
    publics_by_module.reserve(module_names.size());
    for (std::size_t i = 0; i < mf.publics.size(); ++i) {
        const auto& name = mf.publics[i].name;
        // Walk dot positions from rightmost (= longest prefix) to left.
        auto pos = name.rfind('.');
        while (pos != std::string::npos) {
            std::string prefix = name.substr(0, pos);
            if (module_names.count(prefix)) {
                publics_by_module[std::move(prefix)].push_back(i);
                break;
            }
            if (pos == 0) break;
            pos = name.rfind('.', pos - 1);
        }
        // Unmatched publics (e.g. linker-internal $hash$ symbols not
        // tied to any unit) are silently dropped.
    }

    // Group line tables by module_name. The .map emits one line table
    // per (module, source, segment) triple, so the same module shows
    // up in multiple entries (e.g. .text + .data + .pdata). Without
    // collapsing them, publics get re-added once per line-table-of-
    // this-module and symbol counts explode (~57x on Altium AdvPCB).
    std::unordered_map<std::string, std::vector<std::size_t>> lts_by_module;
    lts_by_module.reserve(module_names.size());
    std::vector<std::string> module_order;
    module_order.reserve(module_names.size());
    for (std::size_t i = 0; i < mf.line_tables.size(); ++i) {
        const auto& m = mf.line_tables[i].module_name;
        auto [it, inserted] = lts_by_module.try_emplace(m);
        if (inserted) module_order.push_back(m);
        it->second.push_back(i);
    }

    for (const auto& module_name : module_order) {
        const auto& lt_indices = lts_by_module.at(module_name);
        model::CompileUnit cu;
        cu.source_path = mf.line_tables[lt_indices.front()].source_path;
        const std::uint32_t file_id = sourceFileId(cu.source_path);
        for (std::size_t idx : lt_indices) {
            const auto& lt_i = mf.line_tables[idx];
            for (const auto& l : lt_i.lines) {
                model::LineEntry le;
                le.address = resolveVA(mf, l.segment_id, l.segment_offset);
                le.file_id = file_id;
                le.line    = l.line;
                cu.lines.push_back(le);
            }
        }

        // Public symbols belonging to this module.
        const std::string prefix = module_name + ".";
        const auto bIt = publics_by_module.find(module_name);
        if (bIt != publics_by_module.end()) {
        for (std::size_t idx : bIt->second) {
            const auto& p = mf.publics[idx];
            // Bucketing already guarantees this public's longest
            // module-name prefix equals lt.module_name. Still safe to
            // re-check the literal prefix to handle edge cases.
            if (p.name.rfind(prefix, 0) != 0) continue;
            const std::string local = p.name.substr(prefix.size());
            if (local.empty()) continue;
            if (local.rfind("$pdata$", 0) == 0)  continue;
            if (local.rfind("$unwind$", 0) == 0) continue;
            // Anonymous compiler-internal symbols like ".1" appear
            // after the "Module." prefix is stripped. The full map
            // name has a double-dot (e.g. "Geometry..1"); we detect
            // those by the leading dot in local part.
            if (local.front() == '.')            continue;

            model::Symbol s;
            s.address = resolveVA(mf, p.segment_id, p.segment_offset);
            s.size    = 0;        // .map doesn't give sizes; emitter computes
            s.type    = model::kNoType;

            // Classify by segment class:
            //   CODE                       -> Function
            //   DATA / BSS / TLS / PDATA   -> Variable
            if (const auto* seg = mf.findSegment(p.segment_id)) {
                if (seg->klass == "CODE")
                    s.kind = model::SymbolKind::Function;
                else
                    s.kind = model::SymbolKind::Variable;
            }

            // Naming convention:
            //  - Functions: keep the qualified Pascal name
            //    ("Geometry.Add") so stack traces and `break Unit.Foo`
            //    work nicely.
            //  - Variables: drop the module prefix and store just the
            //    local identifier ("S" instead of "two_units.S").
            //    gdb's Pascal expression parser treats `.` as field
            //    access, so the qualified form fails for VSCode's
            //    -var-create watch expressions.
            s.name = (s.kind == model::SymbolKind::Variable) ? local : p.name;

            cu.symbols.push_back(std::move(s));
        }
        }

        mod.units.push_back(std::move(cu));
    }
}

} // namespace rsm2pdb::map
