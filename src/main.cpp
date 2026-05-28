#include "rsm/rsm_reader.h"
#include "map/map_reader.h"
#include "model/model.h"
#include "dwarf/dwarf_emitter.h"
#include "pe/pe_injector.h"
#include "pe/pe_pdb_injector.h"
#include "pe/prologue.h"
#include "pe/size_sniffer.h"
#include "compose/frame.h"
#include "pdb/pdb_writer.h"

#include <random>
#include <sstream>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Delphi's .map stores source files by bare basename ("Geometry.pas").
// PDB debuggers (cppvsdbg / WinDbg) look up files by the exact path
// they read from the PDB. Resolve the basename to an absolute path
// by probing common locations relative to the .map. Falls back to the
// original string if nothing matches.
static std::string resolveSourcePath(
    const std::string& raw,
    const std::string& map_path,
    const std::vector<std::string>& extra_dirs) {
    namespace fs = std::filesystem;
    if (raw.empty()) return raw;
    fs::path p(raw);
    if (p.is_absolute() && fs::exists(p)) return p.string();

    // Build the search list: explicit --src-search dirs first, then
    // common Delphi project-layout fallbacks relative to the .map.
    std::vector<fs::path> candidates;
    for (const auto& d : extra_dirs) candidates.emplace_back(d);
    const fs::path map_dir = fs::path(map_path).parent_path();
    candidates.push_back(map_dir);                       // alongside .map
    candidates.push_back(map_dir.parent_path());         // one level up
    candidates.push_back(map_dir.parent_path().parent_path()); // two up

    const std::string base = p.filename().string();
    for (const auto& dir : candidates) {
        fs::path cand = dir / base;
        std::error_code ec;
        if (fs::exists(cand, ec)) return fs::canonical(cand, ec).string();
    }
    return raw;
}

static int usage() {
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

static std::string extLower(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e;
}

static int cmdDump(const std::string& path) {
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
                 "error: unrecognized file extension '%s' (want .map or .rsm)\n",
                 ext.c_str());
    return 1;
}

// Diagnostic: cross-check map (authoritative function symbol list) against
// rsm proc records (we parse). VA is the matching key because names use
// different qualification rules in each (map: Unit.Class.Method; rsm:
// Class.Method or just Method). For each missing function we also probe
// the raw rsm bytes to see whether the function's short / class.method
// name is present anywhere — separating "we missed a real record" from
// "this function genuinely has no rsm entry".
static int cmdDiffProcs(const std::string& map_path, const std::string& rsm_path) {
    rsm2pdb::map::Reader mr;
    if (!mr.open(map_path)) {
        std::fprintf(stderr, "error: map: %s\n", mr.error().c_str());
        return 1;
    }
    rsm2pdb::model::Module mod;
    rsm2pdb::map::populate(mr.file(), mod);

    std::vector<std::pair<std::uint64_t, std::string>> map_funcs;
    for (const auto& cu : mod.units) {
        for (const auto& s : cu.symbols) {
            if (s.kind == rsm2pdb::model::SymbolKind::Function) {
                map_funcs.emplace_back(s.address, s.name);
            }
        }
    }

    rsm2pdb::rsm::Reader rr;
    if (!rr.open(rsm_path)) {
        std::fprintf(stderr, "error: rsm: %s\n", rr.error().c_str());
        return 1;
    }

    std::unordered_set<std::uint64_t> rsm_proc_vas;
    rsm_proc_vas.reserve(rr.procedures().size() * 2);
    for (const auto& p : rr.procedures()) rsm_proc_vas.insert(p.address);

    std::unordered_set<std::uint64_t> map_func_vas;
    map_func_vas.reserve(map_funcs.size() * 2);
    for (const auto& f : map_funcs) map_func_vas.insert(f.first);

    std::size_t matched = 0;
    std::vector<std::pair<std::uint64_t, std::string>> missing_list;
    for (const auto& f : map_funcs) {
        if (rsm_proc_vas.count(f.first)) ++matched;
        else missing_list.push_back(f);
    }
    std::size_t orphans = 0;
    for (const auto& p : rr.procedures()) {
        if (!map_func_vas.count(p.address)) ++orphans;
    }

    std::fprintf(stdout, "map functions:   %zu\n", map_funcs.size());
    std::fprintf(stdout, "rsm procs:       %zu\n", rr.procedures().size());
    std::fprintf(stdout, "matched (VA):    %zu (%.1f%% of map functions)\n",
                 matched, 100.0 * matched / std::max<std::size_t>(1, map_funcs.size()));
    std::fprintf(stdout, "missing in rsm:  %zu (%.1f%% of map functions)\n",
                 missing_list.size(),
                 100.0 * missing_list.size() / std::max<std::size_t>(1, map_funcs.size()));
    std::fprintf(stdout, "rsm orphans:     %zu (in rsm but not in map functions)\n",
                 orphans);

    // Load raw rsm bytes for substring probing.
    std::ifstream rsmf(rsm_path, std::ios::binary | std::ios::ate);
    if (!rsmf) {
        std::fprintf(stderr, "error: re-open rsm for raw scan failed\n");
        return 1;
    }
    const auto sz = static_cast<std::streamsize>(rsmf.tellg());
    rsmf.seekg(0);
    std::string raw(static_cast<std::size_t>(sz), '\0');
    rsmf.read(raw.data(), sz);

    // Stride-sample of missing entries so we see RTL, user-code,
    // class-method, and high-VA functions — not just the lowest-VA
    // System.@* block. For each: check whether the short (last
    // segment) and class.method (last two segments) appear as raw
    // bytes in the rsm.
    constexpr std::size_t kSampleN = 60;
    std::vector<std::pair<std::uint64_t, std::string>> sample;
    if (!missing_list.empty()) {
        const std::size_t step = std::max<std::size_t>(1, missing_list.size() / kSampleN);
        for (std::size_t k = 0; k < missing_list.size() && sample.size() < kSampleN; k += step) {
            sample.push_back(missing_list[k]);
        }
    }
    std::fprintf(stdout,
                 "\nStride sample of missing-in-rsm (%zu of %zu, step=%zu); "
                 "RAW = name found as substring in .rsm bytes:\n",
                 sample.size(), missing_list.size(),
                 missing_list.empty() ? 0 : missing_list.size() / kSampleN);
    std::size_t shown = 0;
    std::size_t short_found_count = 0;
    std::size_t cm_found_count    = 0;
    for (const auto& f : sample) {
        const std::string& full = f.second;
        std::string last;
        std::string cls_method;
        const auto p_last = full.rfind('.');
        if (p_last == std::string::npos) {
            last = full;
            cls_method = full;
        } else {
            last = full.substr(p_last + 1);
            const auto p_prev = (p_last == 0) ? std::string::npos
                                              : full.rfind('.', p_last - 1);
            cls_method = (p_prev == std::string::npos)
                ? full
                : full.substr(p_prev + 1);
        }
        const bool last_in_raw = last.size() >= 3 && raw.find(last) != std::string::npos;
        const bool cm_in_raw   = (cls_method != last) && cls_method.size() >= 3
                                 && raw.find(cls_method) != std::string::npos;
        if (last_in_raw) ++short_found_count;
        if (cm_in_raw)   ++cm_found_count;
        std::fprintf(stdout,
                     "  0x%012llx  %-55s  short=%-3s cm=%-3s [%s | %s]\n",
                     static_cast<unsigned long long>(f.first),
                     full.c_str(),
                     last_in_raw ? "YES" : "no",
                     cm_in_raw   ? "YES" : "no",
                     last.c_str(),
                     cls_method.c_str());
        ++shown;
    }
    // Always probe one specific user-code target the user is debugging.
    {
        const std::string target = "PCBCommands_PCB.TPCBCommands.FileSave";
        bool was_in_missing = false;
        for (const auto& f : missing_list) {
            if (f.second == target) {
                was_in_missing = true;
                const bool in_raw = raw.find("TPCBCommands.FileSave") != std::string::npos;
                std::fprintf(stdout,
                             "\nPin-probe %s: missing=YES at 0x%012llx, raw='TPCBCommands.FileSave' present=%s\n",
                             target.c_str(),
                             static_cast<unsigned long long>(f.first),
                             in_raw ? "YES" : "no");
                break;
            }
        }
        if (!was_in_missing) {
            std::fprintf(stdout, "\nPin-probe %s: NOT in missing list "
                         "(either matched or absent from .map)\n", target.c_str());
        }
    }
    std::fprintf(stdout,
                 "\nOf %zu sampled: short-name in .rsm = %zu, class.method in .rsm = %zu\n",
                 shown, short_found_count, cm_found_count);

    return 0;
}

// Diagnostic: for stratified missing procs, locate their raw 0x28-record
// bytes in the .rsm and dump 80 bytes + decode (subtag / VA / trailer
// length). Plus 4 successfully-parsed procs for baseline comparison.
// The intent is to look for patterns: do procedures/functions/virtual/
// overloaded variants encode differently?
static int cmdProbeProcs(const std::string& map_path,
                          const std::string& rsm_path) {
    rsm2pdb::map::Reader mr;
    if (!mr.open(map_path)) {
        std::fprintf(stderr, "error: map: %s\n", mr.error().c_str());
        return 1;
    }
    rsm2pdb::model::Module mod;
    rsm2pdb::map::populate(mr.file(), mod);

    std::vector<std::pair<std::uint64_t, std::string>> map_funcs;
    for (const auto& cu : mod.units) {
        for (const auto& s : cu.symbols) {
            if (s.kind == rsm2pdb::model::SymbolKind::Function) {
                map_funcs.emplace_back(s.address, s.name);
            }
        }
    }

    rsm2pdb::rsm::Reader rr;
    if (!rr.open(rsm_path)) {
        std::fprintf(stderr, "error: rsm: %s\n", rr.error().c_str());
        return 1;
    }
    std::unordered_set<std::uint64_t> rsm_proc_vas;
    rsm_proc_vas.reserve(rr.procedures().size() * 2);
    for (const auto& p : rr.procedures()) rsm_proc_vas.insert(p.address);

    std::vector<std::pair<std::uint64_t, std::string>> missing;
    for (const auto& f : map_funcs) {
        if (!rsm_proc_vas.count(f.first)) missing.push_back(f);
    }
    std::fprintf(stdout, "(missing count: %zu)\n", missing.size());

    std::ifstream rsmf(rsm_path, std::ios::binary | std::ios::ate);
    if (!rsmf) {
        std::fprintf(stderr, "error: re-open rsm failed\n");
        return 1;
    }
    const auto sz = static_cast<std::streamsize>(rsmf.tellg());
    rsmf.seekg(0);
    std::string raw(static_cast<std::size_t>(sz), '\0');
    rsmf.read(raw.data(), sz);

    auto find_pattern = [&](std::string_view name) -> std::vector<std::size_t> {
        std::vector<std::size_t> hits;
        if (name.empty() || name.size() > 200) return hits;
        const std::uint8_t L = static_cast<std::uint8_t>(name.size());
        for (std::size_t i = 0; i + 2 + name.size() < raw.size(); ++i) {
            if (static_cast<std::uint8_t>(raw[i]) == 0x28 &&
                static_cast<std::uint8_t>(raw[i + 1]) == L &&
                std::memcmp(raw.data() + i + 2, name.data(), name.size()) == 0) {
                hits.push_back(i);
                if (hits.size() >= 6) break;
            }
        }
        return hits;
    };

    auto last_two = [](const std::string& n)
        -> std::pair<std::string, std::string> {
        const auto p1 = n.rfind('.');
        if (p1 == std::string::npos) return {n, n};
        const std::string last = n.substr(p1 + 1);
        const auto p2 = (p1 == 0) ? std::string::npos : n.rfind('.', p1 - 1);
        const std::string cm = (p2 == std::string::npos)
            ? n : n.substr(p2 + 1);
        return {cm, last};
    };

    auto print_dump = [&](std::size_t off, std::size_t n) {
        const std::size_t end = std::min(off + n, raw.size());
        for (std::size_t i = off; i < end; ++i) {
            if ((i - off) % 16 == 0) std::fprintf(stdout, "\n    %04zx:", i - off);
            std::fprintf(stdout, " %02x", static_cast<unsigned char>(raw[i]));
        }
        std::fprintf(stdout, "\n");
    };

    // Decode the header following the proc name and report:
    //   subtag (byte at name_end)
    //   the 2 bytes right after it (usually 0x00 0x00)
    //   decoded VA
    //   trailer length: distance from end-of-VA to first valid
    //     sub-record (0x20 / 0x21 with sane shape) OR end marker 0x63
    auto decode_summary = [&](std::size_t off, std::size_t name_len) {
        const std::size_t name_end = off + 2 + name_len;
        if (name_end + 11 > raw.size()) return;
        const std::uint8_t subtag = static_cast<std::uint8_t>(raw[name_end]);
        const std::uint8_t b1     = static_cast<std::uint8_t>(raw[name_end + 1]);
        const std::uint8_t b2     = static_cast<std::uint8_t>(raw[name_end + 2]);
        std::uint32_t va_shifted = 0;
        std::memcpy(&va_shifted, raw.data() + name_end + 7, 4);
        const std::uint64_t va = static_cast<std::uint64_t>(va_shifted) >> 4;

        const std::size_t scan_from = name_end + 11;
        const std::size_t scan_to = std::min(scan_from + 128,
                                             raw.size() > 8 ? raw.size() - 8 : 0);
        std::ptrdiff_t trailer = -1;
        char trailer_kind = '?';
        for (std::size_t s = scan_from; s < scan_to; ++s) {
            const std::uint8_t t = static_cast<std::uint8_t>(raw[s]);
            if (t == 0x63) {
                trailer = static_cast<std::ptrdiff_t>(s - scan_from);
                trailer_kind = 'E';
                break;
            }
            if (t == 0x20 || t == 0x21) {
                const std::uint8_t nl = static_cast<std::uint8_t>(raw[s + 1]);
                if (nl >= 1 && nl <= 32 && s + 2 + nl + 3 < raw.size()) {
                    const std::uint8_t st = static_cast<std::uint8_t>(raw[s + 2 + nl]);
                    if ((st == 0x66 || st == 0x62) &&
                        static_cast<std::uint8_t>(raw[s + 2 + nl + 1]) == 0 &&
                        static_cast<std::uint8_t>(raw[s + 2 + nl + 2]) == 0) {
                        trailer = static_cast<std::ptrdiff_t>(s - scan_from);
                        trailer_kind = (t == 0x21) ? 'P' : 'L';
                        break;
                    }
                }
            }
        }
        std::fprintf(stdout,
                     "    subtag=0x%02x %02x %02x  VA=0x%llx  trailer=",
                     subtag, b1, b2, static_cast<unsigned long long>(va));
        if (trailer < 0) std::fprintf(stdout, "NO_SUB_FOUND_IN_128\n");
        else std::fprintf(stdout, "%td bytes (next='%c')\n",
                          trailer, trailer_kind);
    };

    auto probe = [&](const char* cat,
                      const std::pair<std::uint64_t, std::string>& f) {
        const auto pp = last_two(f.second);
        std::fprintf(stdout, "\n--- [%s] %s (map VA=0x%llx)\n",
                     cat, f.second.c_str(),
                     static_cast<unsigned long long>(f.first));
        const std::string forms[2] = {pp.first, pp.second};
        for (int k = 0; k < 2; ++k) {
            const std::string& form = forms[k];
            auto hits = find_pattern(form);
            std::fprintf(stdout, "  try '%s' (len=%zu): %zu hit(s)\n",
                         form.c_str(), form.size(), hits.size());
            if (hits.empty()) {
                if (k == 1 || pp.first == pp.second) break;
                continue;
            }
            // Prefer the hit whose decoded VA matches map VA.
            std::size_t best = hits.front();
            bool va_match = false;
            for (auto h : hits) {
                const std::size_t ne = h + 2 + form.size();
                if (ne + 11 > raw.size()) continue;
                std::uint32_t vs = 0;
                std::memcpy(&vs, raw.data() + ne + 7, 4);
                if ((static_cast<std::uint64_t>(vs) >> 4) == f.first) {
                    best = h;
                    va_match = true;
                    break;
                }
            }
            std::fprintf(stdout, "  @0x%llx (va_match=%s)",
                         static_cast<unsigned long long>(best),
                         va_match ? "yes" : "no");
            print_dump(best, 80);
            decode_summary(best, form.size());
            break;
        }
    };

    auto first_matching = [&](auto pred)
        -> const std::pair<std::uint64_t, std::string>* {
        for (const auto& f : missing) if (pred(f.second)) return &f;
        return nullptr;
    };
    auto probe_if = [&](const char* cat, auto pred) {
        if (auto* p = first_matching(pred)) probe(cat, *p);
    };

    std::fprintf(stdout, "\n==== Stratified failed-proc samples ====\n");
    probe_if("pin-target", [](const std::string& n){
        return n == "PCBCommands_PCB.TPCBCommands.FileSave";
    });
    probe_if("plain-method", [](const std::string& n){
        return n.find('<') == std::string::npos
            && n.find('{') == std::string::npos
            && std::count(n.begin(), n.end(), '.') >= 2;
    });
    probe_if("constructor", [](const std::string& n){
        return n.find('<') == std::string::npos
            && n.size() > 7 && n.compare(n.size() - 7, 7, ".Create") == 0;
    });
    probe_if("destructor", [](const std::string& n){
        return n.find('<') == std::string::npos
            && n.size() > 8 && n.compare(n.size() - 8, 8, ".Destroy") == 0;
    });
    probe_if("generic-method", [](const std::string& n){
        return n.find('<') != std::string::npos
            || n.find('{') != std::string::npos;
    });
    probe_if("rtl-helper", [](const std::string& n){
        return n.find(".@") != std::string::npos;
    });
    probe_if("free-function", [](const std::string& n){
        const auto p1 = n.find('.');
        if (p1 == std::string::npos) return false;
        if (n.find('.', p1 + 1) != std::string::npos) return false;
        if (p1 + 1 >= n.size()) return false;
        if (n[p1 + 1] == 'T' && p1 + 2 < n.size()
            && std::isupper(static_cast<unsigned char>(n[p1 + 2])))
            return false;
        return true;
    });
    probe_if("setter", [](const std::string& n){
        const auto p = n.rfind(".Set");
        return p != std::string::npos && p + 4 < n.size()
            && n.find('<') == std::string::npos;
    });
    probe_if("getter", [](const std::string& n){
        const auto p = n.rfind(".Get");
        return p != std::string::npos && p + 4 < n.size()
            && n.find('<') == std::string::npos;
    });

    // Baseline: 4 successfully-parsed procs spread across the file.
    std::fprintf(stdout, "\n==== Baseline (4 PARSED procs) ====\n");
    const auto& procs = rr.procedures();
    if (!procs.empty()) {
        const std::size_t idx[4] = {
            0, procs.size() / 3, (2 * procs.size()) / 3, procs.size() - 1
        };
        for (std::size_t i : idx) {
            const auto& p = procs[i];
            std::fprintf(stdout,
                         "\n--- [parsed] %s (VA=0x%llx params=%zu locals=%zu)\n",
                         p.name.c_str(),
                         static_cast<unsigned long long>(p.address),
                         p.params.size(), p.locals.size());
            std::fprintf(stdout, "  @0x%llx",
                         static_cast<unsigned long long>(p.file_offset));
            print_dump(p.file_offset, 80);
            decode_summary(p.file_offset, p.name.size());
        }
    }

    return 0;
}

// Diagnostic: single-pass linear scan to find every plausible proc-record
// candidate, VA-validate against the .map, then aggregate format-statistic
// histograms (proc subtag, trailer length, sub-record subtag values).
// Goal is application rules for a permissive parser, not full format RE.
static int cmdAnalyzeProcs(const std::string& map_path,
                            const std::string& rsm_path) {
    rsm2pdb::map::Reader mr;
    if (!mr.open(map_path)) {
        std::fprintf(stderr, "error: map: %s\n", mr.error().c_str());
        return 1;
    }
    rsm2pdb::model::Module mod;
    rsm2pdb::map::populate(mr.file(), mod);

    std::unordered_set<std::uint64_t> map_func_vas;
    for (const auto& cu : mod.units) {
        for (const auto& s : cu.symbols) {
            if (s.kind == rsm2pdb::model::SymbolKind::Function) {
                map_func_vas.insert(s.address);
            }
        }
    }
    std::fprintf(stdout, "map functions: %zu\n", map_func_vas.size());

    std::ifstream rsmf(rsm_path, std::ios::binary | std::ios::ate);
    if (!rsmf) { std::fprintf(stderr, "error: open rsm failed\n"); return 1; }
    const auto sz = static_cast<std::streamsize>(rsmf.tellg());
    rsmf.seekg(0);
    std::string raw(static_cast<std::size_t>(sz), '\0');
    rsmf.read(raw.data(), sz);
    std::fprintf(stdout, "rsm bytes:     %zu\n", raw.size());

    // Wider printable set for proc-record names (Delphi generics use
    // <>{},  in instantiated names; backticks and spaces also occur).
    auto name_ok_proc = [](const char* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            unsigned char c = static_cast<unsigned char>(p[i]);
            if (std::isalnum(c) || c == '_' || c == '.' || c == '$'
                || c == '<' || c == '>' || c == '{' || c == '}'
                || c == ',' || c == '`' || c == ' ' || c == '@') continue;
            return false;
        }
        return true;
    };
    // Sub-record names are simpler (Pascal identifiers, no generics).
    auto name_ok_sub = [](const char* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            unsigned char c = static_cast<unsigned char>(p[i]);
            if (std::isalnum(c) || c == '_' || c == '.' || c == '$'
                || c == '@') continue;
            return false;
        }
        return true;
    };

    struct ProcInfo {
        std::size_t off;
        std::uint8_t  proc_subtag;
        std::size_t   namelen;
        std::size_t   trailer_len;
        std::uint64_t va;
        std::size_t   param_count;
        std::size_t   local_count;
        bool          walked_ok;
    };
    std::vector<ProcInfo> procs;
    procs.reserve(300000);

    std::map<std::uint8_t, std::size_t> proc_subtag_hist;
    std::map<std::size_t, std::size_t> trailer_len_hist;
    std::map<std::uint8_t, std::size_t> sub_subtag_hist;
    std::map<std::uint8_t, std::size_t> sub_tag_hist;
    // (proc trailer length, param+local count) → count
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> tlen_x_subcount;

    // ---- Pass 1: find VA-validated proc records ----
    std::size_t scanned = 0;
    for (std::size_t i = 0; i + 16 < raw.size(); ) {
        ++scanned;
        if (static_cast<std::uint8_t>(raw[i]) != 0x28) { ++i; continue; }
        const std::uint8_t namelen = static_cast<std::uint8_t>(raw[i + 1]);
        if (namelen < 2 || namelen > 250) { ++i; continue; }
        const std::size_t name_end = i + 2 + namelen;
        if (name_end + 11 >= raw.size()) { ++i; continue; }
        if (!name_ok_proc(raw.data() + i + 2, namelen)) { ++i; continue; }
        // Subtag + two zero bytes (accept ANY subtag; just require the
        // following 2 bytes are zero — that's a much rarer false-positive).
        if (static_cast<std::uint8_t>(raw[name_end + 1]) != 0 ||
            static_cast<std::uint8_t>(raw[name_end + 2]) != 0) { ++i; continue; }
        const std::uint8_t subtag = static_cast<std::uint8_t>(raw[name_end]);
        std::uint32_t va_shifted = 0;
        std::memcpy(&va_shifted, raw.data() + name_end + 7, 4);
        const std::uint64_t va = static_cast<std::uint64_t>(va_shifted) >> 4;
        if (!map_func_vas.count(va)) { ++i; continue; }

        ProcInfo p{};
        p.off         = i;
        p.proc_subtag = subtag;
        p.namelen     = namelen;
        p.va          = va;
        procs.push_back(p);
        proc_subtag_hist[subtag]++;
        i = name_end + 11;
    }

    // ---- Pass 2: trailer length + sub-records per proc ----
    for (auto& p : procs) {
        const std::size_t name_end = p.off + 2 + p.namelen;
        const std::size_t scan_from = name_end + 11;
        const std::size_t scan_to = std::min<std::size_t>(
            scan_from + 192,
            raw.size() > 8 ? raw.size() - 8 : 0);
        // Find first valid sub-record start OR a 0x63 followed by
        // a plausible next-record byte. Don't trust 0x63 alone (false
        // positives inside trailer bytes are common).
        std::size_t first_sub = SIZE_MAX;
        bool ended_immediately = false;
        for (std::size_t s = scan_from; s < scan_to; ++s) {
            const std::uint8_t t = static_cast<std::uint8_t>(raw[s]);
            if (t == 0x20 || t == 0x21) {
                const std::uint8_t nl = static_cast<std::uint8_t>(raw[s + 1]);
                if (nl >= 1 && nl <= 32 && s + 2 + nl + 5 < raw.size()) {
                    const std::size_t bodyAt = s + 2 + nl;
                    if (name_ok_sub(raw.data() + s + 2, nl) &&
                        static_cast<std::uint8_t>(raw[bodyAt + 1]) == 0 &&
                        static_cast<std::uint8_t>(raw[bodyAt + 2]) == 0) {
                        first_sub = s;
                        break;
                    }
                }
            } else if (t == 0x63 && s + 1 < raw.size()) {
                const std::uint8_t n = static_cast<std::uint8_t>(raw[s + 1]);
                // Verify it's a real end: next byte starts a recognizable record.
                if (n == 0x28 || n == 0x20 || n == 0x2A || n == 0xFF || n == 0x9C) {
                    first_sub = s;
                    ended_immediately = true;
                    break;
                }
            }
        }
        if (first_sub == SIZE_MAX) {
            p.walked_ok = false;
            continue;
        }
        p.trailer_len = first_sub - scan_from;
        trailer_len_hist[p.trailer_len]++;

        if (ended_immediately) {
            p.walked_ok = true;
            continue;
        }
        // Walk sub-records assuming the fixed 5-byte payload after name:
        //   <tag 0x20/0x21> <namelen> <name> <subtag> 00 00 <marker> <stack_off>
        std::size_t s = first_sub;
        for (int n = 0; n < 96 && s + 8 < raw.size(); ++n) {
            const std::uint8_t t = static_cast<std::uint8_t>(raw[s]);
            if (t == 0x63) break;
            if (t != 0x20 && t != 0x21) { p.walked_ok = false; break; }
            const std::uint8_t nl = static_cast<std::uint8_t>(raw[s + 1]);
            if (nl == 0 || nl > 64) { p.walked_ok = false; break; }
            const std::size_t bodyAt = s + 2 + nl;
            if (bodyAt + 4 >= raw.size()) { p.walked_ok = false; break; }
            if (static_cast<std::uint8_t>(raw[bodyAt + 1]) != 0 ||
                static_cast<std::uint8_t>(raw[bodyAt + 2]) != 0) {
                p.walked_ok = false;
                break;
            }
            const std::uint8_t st = static_cast<std::uint8_t>(raw[bodyAt]);
            sub_tag_hist[t]++;
            sub_subtag_hist[st]++;
            if (t == 0x21) p.param_count++; else p.local_count++;
            s = bodyAt + 5;
        }
        p.walked_ok = true;
        tlen_x_subcount[{p.trailer_len, p.param_count + p.local_count}]++;
    }

    // ---- Output ----
    std::fprintf(stdout, "\nscanned bytes: %zu\n", scanned);
    std::fprintf(stdout, "VA-confirmed proc candidates: %zu\n", procs.size());

    std::fprintf(stdout, "\n=== Proc-record subtag histogram (top 10) ===\n");
    std::vector<std::pair<std::uint8_t, std::size_t>> psh(
        proc_subtag_hist.begin(), proc_subtag_hist.end());
    std::sort(psh.begin(), psh.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    for (std::size_t k = 0; k < std::min<std::size_t>(10, psh.size()); ++k) {
        std::fprintf(stdout, "  0x%02x: %zu (%.2f%%)\n",
                     psh[k].first, psh[k].second,
                     100.0 * psh[k].second / std::max<std::size_t>(1, procs.size()));
    }

    std::fprintf(stdout, "\n=== Trailer length histogram (top 20) ===\n");
    std::vector<std::pair<std::size_t, std::size_t>> tlh(
        trailer_len_hist.begin(), trailer_len_hist.end());
    std::sort(tlh.begin(), tlh.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    for (std::size_t k = 0; k < std::min<std::size_t>(20, tlh.size()); ++k) {
        std::fprintf(stdout, "  %3zu bytes: %zu procs (%.2f%%)\n",
                     tlh[k].first, tlh[k].second,
                     100.0 * tlh[k].second / std::max<std::size_t>(1, procs.size()));
    }

    std::fprintf(stdout, "\n=== Sub-record tag histogram (0x20=local, 0x21=param) ===\n");
    for (auto& [k, v] : sub_tag_hist) {
        std::fprintf(stdout, "  0x%02x: %zu\n", k, v);
    }

    std::fprintf(stdout, "\n=== Sub-record subtag histogram (top 20) ===\n");
    std::vector<std::pair<std::uint8_t, std::size_t>> ssh(
        sub_subtag_hist.begin(), sub_subtag_hist.end());
    std::sort(ssh.begin(), ssh.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    std::size_t sub_total = 0;
    for (auto& [k, v] : sub_subtag_hist) sub_total += v;
    for (std::size_t k = 0; k < std::min<std::size_t>(20, ssh.size()); ++k) {
        std::fprintf(stdout, "  0x%02x: %zu (%.2f%%)\n",
                     ssh[k].first, ssh[k].second,
                     100.0 * ssh[k].second / std::max<std::size_t>(1, sub_total));
    }

    // Hex sample of one proc per popular trailer length (max 8 samples).
    std::fprintf(stdout, "\n=== Hex samples per popular trailer length ===\n");
    std::unordered_set<std::size_t> shown_tlen;
    for (auto& tt : tlh) {
        if (shown_tlen.size() >= 8) break;
        const std::size_t tlen = tt.first;
        for (const auto& p : procs) {
            if (p.walked_ok && p.trailer_len == tlen
                && shown_tlen.find(tlen) == shown_tlen.end()) {
                shown_tlen.insert(tlen);
                std::fprintf(stdout,
                             "\n  trailer=%zu  VA=0x%llx  params=%zu locals=%zu  name='",
                             tlen, static_cast<unsigned long long>(p.va),
                             p.param_count, p.local_count);
                std::fwrite(raw.data() + p.off + 2, 1, p.namelen, stdout);
                std::fprintf(stdout, "'\n");
                const std::size_t end = std::min(p.off + 80, raw.size());
                for (std::size_t k = p.off; k < end; ++k) {
                    if ((k - p.off) % 16 == 0)
                        std::fprintf(stdout, "    %04zx:", k - p.off);
                    std::fprintf(stdout, " %02x",
                                 static_cast<unsigned char>(raw[k]));
                    if ((k - p.off) % 16 == 15) std::fprintf(stdout, "\n");
                }
                std::fprintf(stdout, "\n");
                break;
            }
        }
    }

    return 0;
}

int main(int argc, char** argv) {
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
        const std::string map_path(argv[2]);
        const std::string out_dir(argv[3]);
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

    if (cmd == "dwarf" && argc == 5) {
        const std::string map_path(argv[2]);
        const std::string input_exe(argv[3]);
        const std::string output_exe(argv[4]);
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

    if (cmd == "pdb" && argc >= 5) {
        // pdb <map> <exe-in-place> <pdb> [--src-search <dir> ...]
        const std::string map_path(argv[2]);
        const std::string input_exe(argv[3]);
        const std::string& output_exe = input_exe;  // in-place injection
        const std::string output_pdb(argv[4]);
        std::vector<std::string> src_search_dirs;
        for (int i = 5; i < argc; ++i) {
            if (std::strcmp(argv[i], "--src-search") == 0 && i + 1 < argc) {
                src_search_dirs.emplace_back(argv[++i]);
            } else {
                std::fprintf(stderr, "error: unknown pdb arg: %s\n", argv[i]);
                return 1;
            }
        }
        if (extLower(map_path) != "map") {
            std::fprintf(stderr, "error: first argument must be a .map file\n");
            return 1;
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
        inputs.publics.reserve(mf.publics.size());
        for (const auto& p : mf.publics) {
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
        std::fprintf(stdout, "publics: %zu emitted, %zu skipped (no PE section)\n",
                     inputs.publics.size(), skipped);

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
        // every resolveFunction call below. See compose::buildMarkerSizes
        // for the cross-unit consistency rules.
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
                // Binary-search the sorted ranges.
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
            // module's C13 subsection. Previously we kept only the
            // last entry, so BPs in any but one .pas per multi-source
            // unit failed to bind silently.
            std::unordered_map<std::string,
                std::vector<const rsm2pdb::map::LineTable*>> by_name;
            for (const auto& lt : mf.line_tables) {
                by_name[lt.module_name].push_back(&lt);
            }
            // Include units that have publics but no line_table at all.
            std::set<std::string> unit_names;
            for (const auto& kv : by_unit) unit_names.insert(kv.first);
            for (const auto& lt : mf.line_tables) unit_names.insert(lt.module_name);

            std::size_t total_fns = 0, total_lines = 0;
            for (const auto& uname : unit_names) {
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
                        // Last function in unit: use unit's range end.
                        const auto* ur = findUnit(r.va);
                        end_va = ur ? ur->va_end : (r.va + 1);
                    }
                    rsm2pdb::pdb::ModuleFunction mf_out;
                    mf_out.name    = r.name;
                    mf_out.segment = seg_idx;
                    mf_out.offset  = static_cast<std::uint32_t>(
                                        rva - pe_sec.virtual_address);
                    mf_out.size    = static_cast<std::uint32_t>(end_va - r.va);

                    // Params + locals from the matching .rsm proc rec.
                    // CodeView S_REGREL32.Offset = 16 + rsm_offset/2,
                    // matching the DWARF emitter's frame_base = rbp+16
                    // convention (see model::Variable docs).
                    //
                    // RSM proc-record offset encoding (empirical from
                    // examples/04_locals reverse engineering):
                    //   - Standalone procs / functions: every param +
                    //     local stack_offset is the real signed offset
                    //     from the frame anchor; formula `16 + off/2`
                    //     gives the correct rbp-relative byte offset.
                    //   - Delphi-x64 places rbp at the BOTTOM of the
                    //     local-area (after `sub rsp, N`), so every
                    //     spilled param + every local sits at a
                    //     POSITIVE offset from rbp. The size of that
                    //     local area (N) is the only run-time piece
                    //     RSM doesn't carry -- we recover it by
                    //     reading the function's prologue out of the
                    //     PE we just parsed.
                    //   - `Self` is the rcx-spill at rbp+sub_rsp+16
                    //     (= first shadow slot). It's flagged in RSM
                    //     by a non-primitive marker (0x29 for class
                    //     methods, 0xd5 for anonymous-method
                    //     closures) and a sentinel stack_offset that
                    //     would never decode meaningfully via the
                    //     general formula. We special-case on both
                    //     marker AND name for belt-and-braces.
                    // Every var is emitted as a real S_REGREL32 typed
                    // `void*` (8-byte hex view). For frameless
                    // thunks (no `sub rsp` in the head) sub_rsp falls
                    // back to 0 and the user sees stale hex addresses
                    // -- more useful than hiding the var entirely.
                    if (have_rsm) {
                        const auto* pr =
                            rsm_reader.findProcedureAt(r.va);
                        if (pr) {
                            // Slice of PE bytes the prologue / disasm-
                            // sniffer in compose::resolveFunction will
                            // read. Empty when the proc's bytes fall
                            // outside the PE; the resolver degrades to
                            // sub_rsp=0 in that case (raw RSM/2).
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
                // units (PCBCommands_PCB.pas + System.Generics.Collections.pas
                // under unit PCBCommands_PCB) keep all their lines.
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
                "modules: %zu, S_GPROC32: %zu, line entries: %zu\n",
                inputs.modules.size(), total_fns, total_lines);
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

    return usage();
}
