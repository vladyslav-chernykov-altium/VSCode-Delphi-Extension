#include "cli/cli.h"
#include "map/map_reader.h"
#include "model/model.h"
#include "rsm/rsm_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rsm2pdb::cli {

int cmdProbeProcs(const std::string& map_path,
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

} // namespace rsm2pdb::cli
