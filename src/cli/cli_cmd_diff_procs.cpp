#include "cli/cli.h"
#include "map/map_reader.h"
#include "model/model.h"
#include "rsm/rsm_reader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rsm2pdb::cli {

int cmdDiffProcs(const std::string& map_path,
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
    // class-method, and high-VA functions -- not just the lowest-VA
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

} // namespace rsm2pdb::cli
