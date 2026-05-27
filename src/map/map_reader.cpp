#include "map/map_reader.h"

#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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
    // Real Delphi .map files are 100s of MB; the previous
    // `ostringstream << rdbuf; ss.str()` path copied the buffer twice.
    // Read directly into a single std::string sized from tellg().
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { error_ = "cannot open " + path; return false; }
    const auto sz = in.tellg();
    in.seekg(0);
    std::string text;
    if (sz > 0) {
        text.resize(static_cast<std::size_t>(sz));
        in.read(text.data(), sz);
        text.resize(static_cast<std::size_t>(in.gcount()));
    } else {
        // Fallback for streams that don't support tellg (pipes, etc.)
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    return parse(text);
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
// Uses std::from_chars to avoid the per-call std::string allocation
// that std::strtoull() forces; on AdvPCB.map this halves .map parsing.
bool parseSegOffset(std::string_view tok,
                    std::uint16_t& segment,
                    std::uint64_t& offset) {
    const auto colon = tok.find(':');
    if (colon == std::string_view::npos) return false;
    const auto seg_s = tok.substr(0, colon);
    const auto off_s = tok.substr(colon + 1);
    auto r1 = std::from_chars(seg_s.data(), seg_s.data() + seg_s.size(),
                              segment, 16);
    if (r1.ec != std::errc{} || r1.ptr != seg_s.data() + seg_s.size()) {
        return false;
    }
    auto r2 = std::from_chars(off_s.data(), off_s.data() + off_s.size(),
                              offset, 16);
    return r2.ec == std::errc{} &&
           r2.ptr == off_s.data() + off_s.size();
}

// Trailing 'H' is sometimes appended to hex numbers in segment overview.
std::uint64_t parseHex(std::string_view tok) {
    if (!tok.empty() && (tok.back() == 'H' || tok.back() == 'h')) {
        tok.remove_suffix(1);
    }
    std::uint64_t v = 0;
    std::from_chars(tok.data(), tok.data() + tok.size(), v, 16);
    return v;
}

// Parse a decimal u32 from string_view without heap allocation.
std::uint32_t parseDec32(std::string_view tok) {
    std::uint32_t v = 0;
    std::from_chars(tok.data(), tok.data() + tok.size(), v, 10);
    return v;
}

// Inline whitespace check -- C library std::isspace consults the
// global locale on every call, which costs measurably on hot loops
// (~3M invocations on real Delphi .map files).
inline bool isWS(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

// Split a line into whitespace-separated tokens.
std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isWS(line[i])) ++i;
        if (i == line.size()) break;
        std::size_t start = i;
        while (i < line.size() && !isWS(line[i])) ++i;
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

    // Section headers are detected by cheap substring match instead of
    // regex. On real Delphi projects (AdvPCB.map = ~3M lines) running
    // 7 std::regex_match calls per data row used to dominate parse
    // time (was ~33s; substring matching pulls it down well under 10s).
    auto trimLeft = [](std::string_view s) {
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    };
    auto stripCRLF = [](std::string_view s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' ||
                              s.back() == '\t' || s.back() == '\n')) {
            s.remove_suffix(1);
        }
        return s;
    };

    auto flushLineTable = [&]() {
        if (!current_table.lines.empty()) {
            file_.line_tables.push_back(std::move(current_table));
        }
        current_table = LineTable{};
        current_table.lines.reserve(64);  // typical Pascal unit
    };
    current_table.lines.reserve(64);

    // On large .map files vector growth dominates (947k publics +
    // 1.8M line entries on AdvPCB). Pre-reserve generously based on
    // file size; overshoots are cheap, reallocations aren't.
    file_.publics.reserve(text.size() / 64);
    file_.module_segments.reserve(8192);

    // Diagnostic: per-state time accumulator. Toggle via env var
    // RSM2PDB_MAP_PROFILE=1 to dump after parse.
    const bool profile_map = std::getenv("RSM2PDB_MAP_PROFILE") != nullptr;
    using clk2 = std::chrono::steady_clock;
    clk2::duration t_hdr{}, t_pub{}, t_lines{}, t_seg{}, t_detailed{};
    std::size_t n_hdr_check = 0;

    // Walk the buffer line-by-line via string_view -- avoids per-line
    // heap allocation of std::getline + std::string. Saves multiple
    // seconds on real-project .map files (~3M lines).
    std::size_t line_no = 0;
    std::size_t cursor = 0;
    const std::string_view buf{text};

    while (cursor < buf.size()) {
        const auto nl = buf.find('\n', cursor);
        const std::size_t end = (nl == std::string_view::npos) ? buf.size() : nl;
        std::string_view line = buf.substr(cursor, end - cursor);
        cursor = (nl == std::string_view::npos) ? buf.size() : nl + 1;
        ++line_no;
        // Strip trailing \r (Windows-style CRLF).
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const auto t_pre_hdr = profile_map ? clk2::now() : clk2::time_point{};

        // Header detection. Data rows always start with whitespace
        // then a hex/decimal digit; headers start with a letter at the
        // left edge after the optional leading whitespace. Branch on
        // the first non-space character so the hot path (99% data rows)
        // only does a single char check.
        const auto lstripped = stripCRLF(trimLeft(line));
        bool is_header = false;
        if (!lstripped.empty()) {
            const char c0 = lstripped.front();
            switch (c0) {
            case 'S':
                if (lstripped.starts_with("Start") &&
                    lstripped.find("Length") != std::string_view::npos) {
                    flushLineTable(); state = State::Segments;
                    is_header = true;
                }
                break;
            case 'D':
                if (lstripped.starts_with("Detailed map of segments")) {
                    flushLineTable(); state = State::DetailedMap;
                    is_header = true;
                }
                break;
            case 'A':
                if (lstripped.starts_with("Address")) {
                    if (lstripped.find("Publics by Name") !=
                        std::string_view::npos) {
                        flushLineTable(); state = State::PublicsByName;
                        is_header = true;
                    } else if (lstripped.find("Publics by Value") !=
                               std::string_view::npos) {
                        flushLineTable(); state = State::PublicsByValue;
                        is_header = true;
                    }
                }
                break;
            case 'B':
                if (lstripped.starts_with("Bound resource files")) {
                    flushLineTable(); state = State::Resources;
                    is_header = true;
                }
                break;
            case 'L':
                if (lstripped.starts_with("Line numbers for ")) {
                    // "Line numbers for <module>(<source>) segment <name>"
                    flushLineTable();
                    std::string_view rest = lstripped;
                    rest.remove_prefix(
                        std::string_view("Line numbers for ").size());
                    const auto lparen = rest.find('(');
                    const auto rparen = rest.rfind(')');
                    const auto seg_kw = rest.rfind(" segment ");
                    if (lparen != std::string_view::npos &&
                        rparen != std::string_view::npos &&
                        seg_kw != std::string_view::npos &&
                        lparen < rparen && rparen < seg_kw) {
                        current_table.module_name =
                            std::string(rest.substr(0, lparen));
                        current_table.source_path = std::string(
                            rest.substr(lparen + 1, rparen - lparen - 1));
                        current_table.segment_name = std::string(rest.substr(
                            seg_kw + std::string_view(" segment ").size()));
                    }
                    state = State::LineNumbers; is_header = true;
                }
                break;
            case 'P':
                if (lstripped.starts_with("Program entry point at ")) {
                    std::string_view rest = lstripped;
                    rest.remove_prefix(
                        std::string_view("Program entry point at ").size());
                    std::uint16_t seg = 0;
                    std::uint64_t off = 0;
                    if (parseSegOffset(rest, seg, off)) {
                        file_.entry_point.has_value = true;
                        file_.entry_point.segment_id = seg;
                        file_.entry_point.segment_offset = off;
                    }
                    state = State::EntryPoint; is_header = true;
                }
                break;
            default:
                break;  // hot path: data row, fall through to dispatch
            }
        }
        if (profile_map) {
            t_hdr += clk2::now() - t_pre_hdr;
            ++n_hdr_check;
        }
        if (is_header) continue;
        const auto t_pre_body = profile_map ? clk2::now() : clk2::time_point{};

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
            // Hand-parsed: skip leading WS, take seg:offset token, skip
            // WS, take everything else as the name. Avoids the
            // tokenize() vector allocation per row (947k publics on
            // AdvPCB.map -> 947k transient vectors gone).
            std::size_t i = 0;
            while (i < line.size() && isWS(line[i])) ++i;
            const std::size_t a = i;
            while (i < line.size() && !isWS(line[i])) ++i;
            const auto t_addr = line.substr(a, i - a);
            Public p{};
            if (!parseSegOffset(t_addr, p.segment_id, p.segment_offset)) {
                break;
            }
            while (i < line.size() && isWS(line[i])) ++i;
            std::size_t name_end = line.size();
            while (name_end > i && isWS(line[name_end - 1])) --name_end;
            if (name_end <= i) break;
            p.name.assign(line.data() + i, name_end - i);
            file_.publics.push_back(std::move(p));
            if (profile_map) t_pub += clk2::now() - t_pre_body;
            break;
        }

        case State::PublicsByValue:
        case State::Resources:
            // Same data we already have, or content we ignore.
            break;

        case State::LineNumbers: {
            // Rows look like:
            //   "    24 0001:00025900    25 0001:0002590E    26 0001:00025917    27 0001:0002591D"
            // Multiple (line, seg:off) tuples per row. Hand-parse
            // without tokenize/string-copy -- this is the hottest loop
            // by far on real Delphi projects (AdvPCB.map has ~1.8M
            // line entries).
            std::size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && isWS(line[i])) ++i;
                if (i >= line.size()) break;
                std::size_t a = i;
                while (i < line.size() && !isWS(line[i])) ++i;
                const auto t_line = line.substr(a, i - a);
                while (i < line.size() && isWS(line[i])) ++i;
                if (i >= line.size()) break;
                a = i;
                while (i < line.size() && !isWS(line[i])) ++i;
                const auto t_addr = line.substr(a, i - a);

                LineRecord lr{};
                lr.line = parseDec32(t_line);
                if (parseSegOffset(t_addr, lr.segment_id, lr.segment_offset)) {
                    current_table.lines.push_back(lr);
                }
            }
            if (profile_map) t_lines += clk2::now() - t_pre_body;
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

    if (profile_map) {
        auto ms = [](clk2::duration d) {
            return std::chrono::duration<double, std::milli>(d).count();
        };
        std::fprintf(stderr,
            "[map-profile] lines=%zu hdr_checks=%zu  hdr=%.0fms pub=%.0fms "
            "lines=%.0fms seg=%.0fms detailed=%.0fms\n",
            line_no, n_hdr_check, ms(t_hdr), ms(t_pub), ms(t_lines),
            ms(t_seg), ms(t_detailed));
    }
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
