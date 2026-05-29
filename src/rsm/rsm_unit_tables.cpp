#include "rsm/rsm_reader.h"
#include "rsm/rsm_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rsm2pdb::rsm {

using namespace detail;

// -- Per-unit type tables -----------------------------------------------
//
// Variables / params / locals carry a per-unit `type_marker` (an even
// byte, 0x02 / 0x04 / 0x06 / ...) that indexes the unit's primary type
// table -- a run of records, one per distinct Pascal type used in the
// unit, in source-declaration order:
//
//   0x66 <namelen u8> <Pascal name> <4-byte hash>
//
// The first record's `name` corresponds to marker 0x02, the second to
// 0x04, etc. (so `index = marker/2 - 1`, `marker = 2*(index+1)`).
//
// Locating each unit's primary table:
//   1. Find every "unit anchor" -- `02 <namelen> <unit name>` followed
//      (within ~200 bytes) by `02 70 <namelen> <source file ending in
//      .pas / .dpr / .inc / .tmp>`.
//   2. For each anchor, the next-following 0x66-run is the unit's
//      primary type table.
//   3. For each variable, the enclosing unit is the closest-prior
//      anchor; look up its primary table and decode the marker.
//
// Variables / functions in units whose anchor we don't find (some RTL
// units use a layout the strict heuristic doesn't match) simply get
// no `pascal_type` -- callers fall back to byte-size-based typing.
//
// Trailing bookkeeping: this pass also builds var_by_va_ / proc_by_va_
// (since both variables_ and procedures_ are populated by then), and
// mirrors unit_anchors / primary_tables into the member-side
// unit_anchor_offsets_ / primary_table_by_anchor_ structures that
// downstream passes (and external callers via unitAnchorFor /
// findAggregateInUnit / primitiveNameForMarker) rely on.
void Reader::scanPerUnitTypeTables(const std::string& buf) {
    auto isPrintAscii = [&](const char* p, std::size_t n) {
        for (std::size_t k = 0; k < n; ++k) {
            const auto c = static_cast<std::uint8_t>(p[k]);
            if (c < 0x20 || c >= 0x7F) return false;
        }
        return true;
    };
    auto endsWith = [](const char* p, std::size_t n, const char* suf,
                       std::size_t sufN) {
        return n >= sufN &&
               std::memcmp(p + n - sufN, suf, sufN) == 0;
    };

    struct UnitAnchor { std::size_t file_offset; std::string name; };
    std::vector<UnitAnchor> unit_anchors;
    const std::size_t kVarPayloadHdr = 0x420;
    for (std::size_t a = kVarPayloadHdr; a + 60 < buf.size(); ) {
        if (static_cast<std::uint8_t>(buf[a]) != 0x02) { ++a; continue; }
        const auto nl = static_cast<std::uint8_t>(buf[a + 1]);
        if (nl < 2 || nl > 40 ||
            !isPrintAscii(buf.data() + a + 2, nl)) {
            ++a; continue;
        }
        // Source-file signature `<prefix> 70 <fnl> <printable>.{ext}`
        // within the next 200 bytes. Prefix is 0x02 for .dpr-style
        // self-anchors or 0x3c for .pas anchors (Shapes / Items /
        // Layout in 09_cross_unit; AdvPCB's RTL units commonly).
        // Without 0x3c acceptance multi-unit projects only register
        // the .dpr anchor.
        const std::size_t scan_lo = a + 2 + nl;
        const std::size_t scan_hi = std::min(buf.size(), scan_lo + 200);
        bool matched = false;
        for (std::size_t s = scan_lo; s + 8 < scan_hi; ++s) {
            const auto b0 = static_cast<std::uint8_t>(buf[s]);
            if ((b0 != 0x02 && b0 != 0x3c)
                || static_cast<std::uint8_t>(buf[s + 1]) != 0x70) continue;
            const auto fnl = static_cast<std::uint8_t>(buf[s + 2]);
            if (fnl < 4 || fnl > 80) continue;
            if (s + 3 + fnl > buf.size()) continue;
            const char* fp = buf.data() + s + 3;
            if (!isPrintAscii(fp, fnl)) continue;
            if (endsWith(fp, fnl, ".pas", 4) ||
                endsWith(fp, fnl, ".dpr", 4) ||
                endsWith(fp, fnl, ".inc", 4) ||
                endsWith(fp, fnl, ".tmp", 4)) {
                matched = true;
                break;
            }
        }
        if (matched) {
            unit_anchors.push_back({
                a, std::string(buf.data() + a + 2, nl)});
            a += 2 + nl;
        } else {
            ++a;
        }
    }

    // Build a primary type table per unit anchor by scanning the bytes
    // between this anchor and the next. The table is a sequence of
    // `0x66 <namelen> <name> <4-byte hash>` records interleaved with
    // `0x67` import/function references (same shape, different tag);
    // collect the 0x66's and skip past 0x67's. The `63 64 <fnl>
    // <imported_unit_name> 00 00 00` separator (between imported-unit
    // blocks in multi-uses tables) is walked past too. marker N is
    // the (N/2)-th entry.
    struct TypeTable {
        std::size_t                start;
        std::vector<UnitTypeEntry> entries;
    };
    std::vector<TypeTable> primary_tables;
    primary_tables.reserve(unit_anchors.size());
    for (std::size_t ua = 0; ua < unit_anchors.size(); ++ua) {
        const std::size_t lo = unit_anchors[ua].file_offset;
        const std::size_t hi = (ua + 1 < unit_anchors.size())
            ? unit_anchors[ua + 1].file_offset
            : buf.size();

        TypeTable tab;
        tab.start = lo;
        const std::size_t scan_lo = lo + 0x20;  // past unit name etc.
        for (std::size_t cur = scan_lo; cur + 10 < hi; ) {
            const auto b0 = static_cast<std::uint8_t>(buf[cur]);
            if (b0 == 0x63 && cur + 4 < hi
                && static_cast<std::uint8_t>(buf[cur + 1]) == 0x64) {
                const auto fnl = static_cast<std::uint8_t>(buf[cur + 2]);
                if (fnl >= 2 && fnl <= 64
                    && cur + 3 + fnl + 3 <= hi
                    && isPrintAscii(buf.data() + cur + 3, fnl)) {
                    cur += 3 + fnl + 3;
                    continue;
                }
            }
            if (b0 != kVarPayloadSubTag0 && b0 != 0x67) {
                if (!tab.entries.empty()) break;
                if (cur - scan_lo > 500) break;
                ++cur;
                continue;
            }
            const auto c_nl = static_cast<std::uint8_t>(buf[cur + 1]);
            if (c_nl < 2 || c_nl > 80 ||
                cur + 2 + c_nl + 4 > hi ||
                !isPrintAscii(buf.data() + cur + 2, c_nl)) {
                if (!tab.entries.empty()) break;
                ++cur;
                continue;
            }
            if (b0 == kVarPayloadSubTag0) {
                UnitTypeEntry e;
                e.name.assign(buf.data() + cur + 2, c_nl);
                const std::size_t h = cur + 2 + c_nl;
                e.hash4 = static_cast<std::uint8_t>(buf[h])
                       | (static_cast<std::uint8_t>(buf[h + 1]) << 8)
                       | (static_cast<std::uint8_t>(buf[h + 2]) << 16)
                       | (static_cast<std::uint8_t>(buf[h + 3]) << 24);
                tab.entries.push_back(std::move(e));
            }
            cur += 2 + c_nl + 4;
        }
        primary_tables.push_back(std::move(tab));
    }

    // Anchor offset -> primary table.
    std::unordered_map<std::size_t, const TypeTable*> primary_table;
    for (std::size_t k = 0; k < unit_anchors.size(); ++k) {
        if (!primary_tables[k].entries.empty()) {
            primary_table[unit_anchors[k].file_offset] = &primary_tables[k];
        }
    }

    // Stamp pascal_type on every Variable / param / local via its
    // enclosing unit's primary table.
    auto resolvePascalType = [&](Variable& v) {
        if (!v.is_primitive || v.type_marker == 0
            || (v.type_marker & 1) != 0) return;
        const std::size_t idx =
            static_cast<std::size_t>(v.type_marker) / 2;
        if (idx == 0) return;
        auto it = std::upper_bound(
            unit_anchors.begin(), unit_anchors.end(), v.file_offset,
            [](std::size_t v_, const UnitAnchor& a) {
                return v_ < a.file_offset;
            });
        if (it == unit_anchors.begin()) return;
        --it;
        auto pit = primary_table.find(it->file_offset);
        if (pit == primary_table.end() || pit->second == nullptr) return;
        const auto& entries = pit->second->entries;
        if (idx > entries.size()) return;
        v.pascal_type = entries[idx - 1].name;
    };
    for (auto& v : variables_) resolvePascalType(v);
    for (auto& proc : procedures_) {
        for (auto& v : proc.params) resolvePascalType(v);
        for (auto& v : proc.locals) resolvePascalType(v);
    }

    std::fprintf(stderr,
                 "[rsm] type tables: %zu anchors, %zu primaries, "
                 "%zu globals typed\n",
                 unit_anchors.size(), primary_table.size(),
                 [&] {
                     std::size_t n = 0;
                     for (const auto& v : variables_)
                         if (!v.pascal_type.empty()) ++n;
                     return n;
                 }());

    // VA -> index maps for O(1) lookup. Below-image-base addresses are
    // the bogus VAs the variable scanner picks up from procedure-
    // internal byte sequences.
    var_by_va_.reserve(variables_.size());
    for (std::size_t k = 0; k < variables_.size(); ++k) {
        const auto& v = variables_[k];
        if (v.address >= 0x00400000) var_by_va_.emplace(v.address, k);
    }
    proc_by_va_.reserve(procedures_.size());
    for (std::size_t k = 0; k < procedures_.size(); ++k) {
        proc_by_va_.emplace(procedures_[k].address, k);
    }

    // Mirror local unit_anchors / primary_tables into the member-side
    // structures the aggregate parser + downstream callers
    // (unitAnchorFor, primitiveNameForMarker) depend on.
    unit_anchor_offsets_.clear();
    unit_anchor_offsets_.reserve(unit_anchors.size());
    for (const auto& ua : unit_anchors) {
        unit_anchor_offsets_.push_back(ua.file_offset);
    }
    std::sort(unit_anchor_offsets_.begin(), unit_anchor_offsets_.end());

    primary_table_by_anchor_.clear();
    for (std::size_t k = 0; k < unit_anchors.size(); ++k) {
        if (k >= primary_tables.size()) break;
        const auto& tt = primary_tables[k];
        if (tt.entries.empty()) continue;
        primary_table_by_anchor_[unit_anchors[k].file_offset] = tt.entries;
    }

    // Stamp unit_anchor_offset on every Variable so downstream pipelines
    // (compose / PDB) don't have to re-run upper_bound.
    for (auto& v : variables_) {
        v.unit_anchor_offset = unitAnchorFor(v.file_offset);
    }
    for (auto& proc : procedures_) {
        for (auto& v : proc.params) {
            v.unit_anchor_offset = unitAnchorFor(v.file_offset);
        }
        for (auto& v : proc.locals) {
            v.unit_anchor_offset = unitAnchorFor(v.file_offset);
        }
    }
}

} // namespace rsm2pdb::rsm
