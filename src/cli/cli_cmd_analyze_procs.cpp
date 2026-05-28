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
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rsm2pdb::cli {

int cmdAnalyzeProcs(const std::string& map_path,
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

} // namespace rsm2pdb::cli
