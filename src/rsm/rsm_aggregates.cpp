#include "rsm/rsm_reader.h"
#include "rsm/rsm_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rsm2pdb::rsm {

using namespace detail;

// -- Aggregate types (Step 11b Phase B.1) -------------------------
//
// Walk the metadata stream collecting 0x2a / 0x2c / 0x25 records
// into `aggregates_`. See rsm-format.txt 2026-05-28 entries for
// the full encoding.
//
// Two-pass approach (the records live in two physically separate
// clusters within the unit's metadata stream, linked by
// parent_hash):
//   pass 1 -- collect 0x2a records into aggregates_ keyed by
//             own_hash; collect dangling 0x2c / 0x25 records
//             into temporary maps keyed by parent_hash.
//   pass 2 -- attach the dangling records to their parent
//             aggregate; infer kind (record / class / enum)
//             from the attached storage bytes / entries.
//
// We DELIBERATELY don't trust the 0x2a record's `kind` byte to
// discriminate -- empirically TBig (record with 40 fields) and
// TColor (enum) share the same kind byte 0x00, so kind must be
// inferred from what attaches to the type.
void Reader::scanAggregateTypes(const std::string& buf) {
    std::unordered_map<std::uint16_t,
                       std::vector<FieldEntry>> pending_fields;
    // Per-unit-scoped pending fields: same key shape as the
    // aggr_by_unit_hash_ map so we can attach fields without
    // tripping on cross-unit hash collisions (System.TTextRec's
    // own_hash 0x0405 vs PCBCommands_PCB.TPCBCommands's
    // linked_hash 0x0405 -- both legitimate, both real). The
    // global pending_fields map above stays for the rare case
    // where we can't determine a unit anchor.
    std::map<std::pair<std::uint64_t, std::uint16_t>,
             std::vector<FieldEntry>> pending_fields_by_unit;
    std::unordered_map<std::uint16_t,
                       std::vector<EnumEntry>>  pending_enums;
    // Per-unit-scoped pending enums (same rationale as
    // pending_fields_by_unit -- multiple units can declare
    // enums with the same per-unit own_hash, e.g.
    // Shapes.TColor and some System enum both at 0x0405).
    std::map<std::pair<std::uint64_t, std::uint16_t>,
             std::vector<EnumEntry>> pending_enums_by_unit;
    // For class header back-references (base_hash, total_size).
    // The class header's `47 00 10 00 00 <own_hash u16>` pattern
    // lets us pick out base-class refs even though they're in a
    // separate physical position from the 0x2a record.
    struct ClassHeader {
        std::uint16_t own_hash;
        std::uint16_t base_hash;     // 0 if "no explicit base"
    };
    std::vector<ClassHeader> class_headers;

    auto readU16LE = [&](std::size_t off) -> std::uint16_t {
        return static_cast<std::uint8_t>(buf[off])
             | (static_cast<std::uint8_t>(buf[off + 1]) << 8);
    };

    const std::size_t scan_lo = std::min<std::size_t>(0x420, buf.size());
    std::size_t i = scan_lo;
    while (i + 8 < buf.size()) {
        const auto tag = static_cast<std::uint8_t>(buf[i]);

        // 0x2a -- type record: 2a <namelen> <name> <kind u8> 00 00 <own_hash u16> [extras]
        if (tag == 0x2a) {
            const auto nl = static_cast<std::uint8_t>(buf[i + 1]);
            if (nl < 2 || nl > 64
                || i + 2 + nl + 6 > buf.size()) { ++i; continue; }
            if (!isPrintableName(buf.data() + i + 2, nl)) { ++i; continue; }
            // Validate the `<kind> 00 00` shape at +2+nl.
            const std::size_t at = i + 2 + nl;
            if (static_cast<std::uint8_t>(buf[at + 1]) != 0x00
                || static_cast<std::uint8_t>(buf[at + 2]) != 0x00) {
                ++i; continue;
            }
            AggregateType a;
            a.name        = std::string(buf.data() + i + 2, nl);
            a.own_hash    = readU16LE(at + 3);
            // Linked hash: when kind has bit 0x80 set the
            // 0x2a record carries a secondary hash at +7..+8
            // that points at the actual class-header / fields
            // anchor. Used by AdvPCB-style classes where the
            // NAME record sits separately from the FIELDS.
            // For other kinds (or when +8 is out-of-buffer)
            // leave at 0.
            const auto kind_byte =
                static_cast<std::uint8_t>(buf[at]);
            if ((kind_byte & 0x80) != 0 && at + 8 < buf.size()) {
                a.linked_hash = readU16LE(at + 7);
            }
            a.file_offset = i;
            // Pin the aggregate to its enclosing unit anchor.
            // Closest-prior anchor via upper_bound on sorted
            // offsets (Phase B.4). Without a unit anchor the
            // aggregate is unscoped; downstream lookups fall
            // back to the global last-wins index.
            auto uit = std::upper_bound(
                unit_anchor_offsets_.begin(),
                unit_anchor_offsets_.end(),
                static_cast<std::uint64_t>(i));
            a.unit_anchor_offset =
                (uit == unit_anchor_offsets_.begin())
                    ? 0
                    : *(uit - 1);
            // Provisional kind = Unknown; refined in pass 2.
            aggregates_.push_back(std::move(a));
            i = at + 5;
            continue;
        }

        // 0x2c -- field record: 2c <namelen> <name> 00 <storage> 00
        //                       <type-ref 1/2B> <offset 1/2B>
        //                       <internals 8/13B> <parent_hash u16> ff
        if (tag == 0x2c) {
            const auto nl = static_cast<std::uint8_t>(buf[i + 1]);
            if (nl < 1 || nl > 64
                || i + 2 + nl + 6 > buf.size()) { ++i; continue; }
            if (!isPrintableName(buf.data() + i + 2, nl)) { ++i; continue; }
            const std::size_t at = i + 2 + nl;
            // Anchor on the `00 <storage> 00` prefix.
            if (static_cast<std::uint8_t>(buf[at]) != 0x00
                || static_cast<std::uint8_t>(buf[at + 2]) != 0x00) {
                ++i; continue;
            }
            const std::uint8_t storage =
                static_cast<std::uint8_t>(buf[at + 1]);
            std::size_t cur = at + 3;
            // Type-ref discriminator: small even byte (< 0x40) =
            // primitive marker; else 2-byte hash. We don't dare
            // assume; check both bytes.
            FieldEntry fe;
            fe.name    = std::string(buf.data() + i + 2, nl);
            fe.storage = storage;

            const auto b0 = static_cast<std::uint8_t>(buf[cur]);
            bool is_primitive_ref = (b0 != 0
                                     && b0 < 0x40
                                     && (b0 & 0x01) == 0);
            if (is_primitive_ref) {
                fe.primitive_marker = b0;
                cur += 1;
            } else {
                if (cur + 2 > buf.size()) { ++i; continue; }
                fe.type_hash = readU16LE(cur);
                cur += 2;
            }

            // Offset: 1-byte form if LSB=0, else 2-byte LE u16
            // decoded as `real = (v - 1) / 4`.
            if (cur >= buf.size()) { ++i; continue; }
            const auto off_b0 = static_cast<std::uint8_t>(buf[cur]);
            if ((off_b0 & 0x01) == 0) {
                fe.offset = off_b0 / 2;
                cur += 1;
            } else {
                if (cur + 2 > buf.size()) { ++i; continue; }
                const std::uint16_t v = readU16LE(cur);
                fe.offset = (static_cast<std::uint32_t>(v) - 1) / 4;
                cur += 2;
            }

            // Walk to terminating `ff`. The `<parent_hash u16> ff`
            // sequence is canonical -- internals are 8 or 13 bytes
            // depending on the type-ref form, but defensively we
            // scan up to 24 bytes ahead for an `ff` and capture the
            // u16 immediately before it as parent_hash.
            std::size_t walk_end = std::min(cur + 24, buf.size());
            std::size_t ff_at = SIZE_MAX;
            for (std::size_t j = cur; j < walk_end; ++j) {
                if (static_cast<std::uint8_t>(buf[j]) == 0xff) {
                    ff_at = j;
                    break;
                }
            }
            if (ff_at == SIZE_MAX || ff_at < cur + 2) { ++i; continue; }
            const std::uint16_t parent = readU16LE(ff_at - 2);
            // Stamp the field with its enclosing unit anchor so
            // we can attach by (unit, parent_hash) and avoid
            // cross-unit hash collisions (System.TTextRec
            // 0x0405 vs PCBCommands_PCB.TPCBCommands linked
            // 0x0405). Field's file_offset = `i` (start of the
            // 0x2c record).
            const std::uint64_t fua = unitAnchorFor(
                static_cast<std::uint64_t>(i));
            if (fua != 0) {
                pending_fields_by_unit[{fua, parent}]
                    .push_back(std::move(fe));
            } else {
                pending_fields[parent].push_back(std::move(fe));
            }
            i = ff_at + 1;
            continue;
        }

        // 0x25 -- enum entry: 25 <namelen> <name> 0a 00 00 <parent_hash u16> 00 00 <ord_x2 u8>
        if (tag == 0x25) {
            const auto nl = static_cast<std::uint8_t>(buf[i + 1]);
            if (nl < 1 || nl > 64
                || i + 2 + nl + 9 > buf.size()) { ++i; continue; }
            if (!isPrintableName(buf.data() + i + 2, nl)) { ++i; continue; }
            const std::size_t at = i + 2 + nl;
            // Anchor on `0a 00 00`.
            if (static_cast<std::uint8_t>(buf[at])     != 0x0a
                || static_cast<std::uint8_t>(buf[at + 1]) != 0x00
                || static_cast<std::uint8_t>(buf[at + 2]) != 0x00) {
                ++i; continue;
            }
            const std::uint16_t parent = readU16LE(at + 3);
            // Validate the trailing `00 00 <ord_x2>` shape.
            if (static_cast<std::uint8_t>(buf[at + 5]) != 0x00
                || static_cast<std::uint8_t>(buf[at + 6]) != 0x00) {
                ++i; continue;
            }
            const auto ord_x2 = static_cast<std::uint8_t>(buf[at + 7]);
            EnumEntry e;
            e.name    = std::string(buf.data() + i + 2, nl);
            e.ordinal = static_cast<std::int64_t>(ord_x2) / 2;
            // Stamp the enum entry with its enclosing unit
            // anchor so the per-unit attach defeats cross-
            // unit hash collisions (Shapes.TColor 0x0405 vs
            // System.SomeEnum 0x0405).
            const std::uint64_t eua = unitAnchorFor(
                static_cast<std::uint64_t>(i));
            if (eua != 0) {
                pending_enums_by_unit[{eua, parent}]
                    .push_back(std::move(e));
            } else {
                pending_enums[parent].push_back(std::move(e));
            }
            i = at + 8;
            continue;
        }

        // Class header signature: 47 00 10 00 00 <own_hash u16>
        // The intermediate "vtable size" block has a variable
        // length between fixtures (07_records: 9 bytes, 08:
        // 10 bytes when the class declares property accessor
        // methods). Robust strategy: scan forward for the
        // `25 3f 00` field-list anchor and pin the base-class
        // 2-byte slot at fl_at - 10 (always immediately before
        // the canonical 8-byte tail `00 NN .. fe 00 NN`).
        // See rsm-format.txt 2026-05-28 inheritance entry.
        if (tag == 0x47 && i + 24 < buf.size()
            && static_cast<std::uint8_t>(buf[i + 1]) == 0x00
            && static_cast<std::uint8_t>(buf[i + 2]) == 0x10
            && static_cast<std::uint8_t>(buf[i + 3]) == 0x00
            && static_cast<std::uint8_t>(buf[i + 4]) == 0x00) {
            ClassHeader ch{};
            ch.own_hash  = readU16LE(i + 5);
            // Scan forward up to 80 bytes for the field-list
            // anchor; the base-class slot is at -10/-9.
            std::size_t fl_at = SIZE_MAX;
            const std::size_t scan_end =
                std::min(i + 80, buf.size() - 3);
            for (std::size_t j = i + 7; j < scan_end; ++j) {
                if (static_cast<std::uint8_t>(buf[j])     == 0x25
                    && static_cast<std::uint8_t>(buf[j + 1]) == 0x3f
                    && static_cast<std::uint8_t>(buf[j + 2]) == 0x00) {
                    fl_at = j;
                    break;
                }
            }
            if (fl_at != SIZE_MAX && fl_at >= 10) {
                ch.base_hash = readU16LE(fl_at - 10);
            }
            class_headers.push_back(ch);
            i += 7;
            continue;
        }

        ++i;
    }

    // Build the own_hash -> aggregate index map.
    //
    // own_hash is unit-local, not globally unique -- RTL units can
    // declare types with the same hash as our user units (TColor
    // in records.dpr collides with at least one System-unit type
    // at hash 0x1a6d). Two indexes:
    //
    //   aggr_by_hash_       -- last-wins by hash alone. Used when
    //                          caller has no unit context.
    //                          (User units link last, so the
    //                          user aggregate wins on practical
    //                          fixtures.)
    //
    //   aggr_by_unit_hash_  -- precise by (unit_anchor_offset,
    //                          hash). Used when the caller knows
    //                          which unit owns the lookup. The
    //                          pascal_type resolver below uses
    //                          this when a variable's file_offset
    //                          falls inside a unit anchor.
    aggr_by_hash_.reserve(aggregates_.size());
    aggr_by_unit_hash_.reserve(aggregates_.size());
    for (std::size_t k = 0; k < aggregates_.size(); ++k) {
        const auto& a = aggregates_[k];
        aggr_by_hash_[a.own_hash] = k;
        if (a.unit_anchor_offset != 0) {
            aggr_by_unit_hash_[
                UnitHashKey{a.unit_anchor_offset, a.own_hash}] = k;
        }
        // ALSO index under linked_hash so variables that
        // reference the class via the secondary hash (Self of
        // AdvPCB TPCBCommands has aggregate_hash 0x0405 while
        // the named 0x2a sits at own_hash 0xd0cf with
        // linked_hash 0x0405) resolve to the same aggregate.
        // Don't overwrite an existing primary-hash entry --
        // a real own_hash always wins over a linked_hash.
        if (a.linked_hash != 0
            && a.linked_hash != a.own_hash) {
            if (aggr_by_hash_.find(a.linked_hash)
                == aggr_by_hash_.end()) {
                aggr_by_hash_[a.linked_hash] = k;
            }
            if (a.unit_anchor_offset != 0) {
                UnitHashKey k2{a.unit_anchor_offset, a.linked_hash};
                if (aggr_by_unit_hash_.find(k2)
                    == aggr_by_unit_hash_.end()) {
                    aggr_by_unit_hash_[k2] = k;
                }
            }
        }
    }

    // class_hashes -- set of all 0x47 own_hashes. Used both
    // by the X.3 same-unit chaining below and by the
    // classifier further down.
    std::unordered_set<std::uint16_t> class_hashes;
    class_hashes.reserve(class_headers.size());
    for (const auto& ch : class_headers) class_hashes.insert(ch.own_hash);

    // Phase X.3: same-unit hash chaining. A class declared
    // within one unit shows up under FOUR different hashes:
    //
    //   NAME       = 0x2a own_hash
    //   LINKED     = 0x2a's secondary hash (+7/+8 from kind,
    //                only when kind & 0x80)
    //   CLS_HEADER = 0x47 own_hash
    //   FLD_PARENT = 0x2c parent_hash (the parent of fields)
    //
    // Two encoding patterns observed across fixtures:
    //
    //   AdvPCB (kind=0xa8, single-record):
    //       LINKED == CLS_HEADER == FLD_PARENT.
    //       The 0x47 header + fields all share one own_hash;
    //       the 0x2a record's linked_hash points straight at
    //       it. Already handled by the linked_hash indexing
    //       above (CLS_HEADER own_hash gets registered).
    //
    //   09_cross_unit (kind=0xa8, split layout):
    //       LINKED.low == CLS_HEADER.low  (high bytes differ)
    //       FLD_PARENT = (CLS_HEADER.low << 8) | 0x08
    //       e.g. TItem: LINKED 0x0712, CLS 0xb112, FLD 0x1208.
    //
    // For the latter, scan class_headers and link them to
    // pending 0x2a aggregates by low-byte match in same unit.
    // Then ALSO register the aggregate under both the
    // CLS_HEADER own_hash AND the derived FLD_PARENT so the
    // field-attach loop below finds them.
    for (std::size_t k = 0; k < aggregates_.size(); ++k) {
        auto& a = aggregates_[k];
        if (a.linked_hash == 0 || a.unit_anchor_offset == 0) continue;
        // Universal formula derived from per-unit observation:
        //     FLD_PARENT = ((LINKED & 0xff) << 8) | 0x08
        // holds for BOTH classes and records:
        //   records.dpr TPoint    : own=0x1a71 (records.dpr
        //     fixture has the older format and doesn't use
        //     linked_hash chaining -- the direct-attach path
        //     still handles it).
        //   09 Items.TItem        : LINKED=0x0712 -> FLD 0x1208 ✓
        //   09 Layout.TLayout     : LINKED=0x0710 -> FLD 0x1008 ✓
        //   09 Shapes.TPoint      : LINKED=0x0706 -> FLD 0x0608 ✓
        //   09 Shapes.TSize       : LINKED=0x8708 -> FLD 0x0808 ✓
        //   AdvPCB TPCBCommands   : LINKED == own_hash of class
        //     header (already aliased by the prior linked_hash
        //     loop) -- formula yields FLD 0x0508 which the
        //     attach loop won't find any fields for, harmless.
        const std::uint16_t fld_parent =
            static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(a.linked_hash & 0xff) << 8)
                | 0x08u);
        UnitHashKey uk_fld{a.unit_anchor_offset, fld_parent};
        if (aggr_by_unit_hash_.find(uk_fld)
            == aggr_by_unit_hash_.end()) {
            aggr_by_unit_hash_[uk_fld] = k;
        }
        if (aggr_by_hash_.find(fld_parent) == aggr_by_hash_.end()) {
            aggr_by_hash_[fld_parent] = k;
        }
        // For CLASSES specifically: 09-style classes link the
        // 0x2a record's linked_hash to a 0x47 class header via
        // low-byte match (linked.low == header.low). Find it.
        if (class_hashes.count(a.linked_hash)) {
            // AdvPCB direct match (linked == cls_own) -- the
            // aggregate is already aliased to that hash and
            // gets force-classified below.
            a.kind = AggregateKind::Class;
        } else {
            const std::uint8_t link_low = a.linked_hash & 0xff;
            std::uint16_t cls_own = 0;
            for (const auto& ch : class_headers) {
                if ((ch.own_hash & 0xff) != link_low) continue;
                if (cls_own == 0) {
                    cls_own = ch.own_hash;
                } else if (cls_own != ch.own_hash) {
                    cls_own = 0;
                    break;
                }
            }
            if (cls_own != 0) {
                UnitHashKey uk_cls{a.unit_anchor_offset, cls_own};
                if (aggr_by_unit_hash_.find(uk_cls)
                    == aggr_by_unit_hash_.end()) {
                    aggr_by_unit_hash_[uk_cls] = k;
                }
                if (aggr_by_hash_.find(cls_own) == aggr_by_hash_.end()) {
                    aggr_by_hash_[cls_own] = k;
                }
                a.kind = AggregateKind::Class;
            }
        }
    }

    // Attach pending fields by (unit, parent_hash) -- this is
    // the precise version that survives cross-unit hash
    // collisions (e.g. System.TTextRec.own_hash collides with
    // PCBCommands_PCB.TPCBCommands.linked_hash, both 0x0405).
    for (auto& [k, v] : pending_fields_by_unit) {
        auto it = aggr_by_unit_hash_.find(
            UnitHashKey{k.first, k.second});
        if (it == aggr_by_unit_hash_.end()) continue;
        auto& a = aggregates_[it->second];
        a.fields = std::move(v);
    }
    // Fallback: any fields we couldn't tie to a unit anchor get
    // the global last-wins attach (rare; matches old behaviour).
    for (auto& [h, v] : pending_fields) {
        auto it = aggr_by_hash_.find(h);
        if (it == aggr_by_hash_.end()) continue;
        auto& a = aggregates_[it->second];
        if (a.fields.empty()) a.fields = std::move(v);
    }
    for (auto& [k, v] : pending_enums_by_unit) {
        auto it = aggr_by_unit_hash_.find(
            UnitHashKey{k.first, k.second});
        if (it == aggr_by_unit_hash_.end()) continue;
        auto& a = aggregates_[it->second];
        a.enum_entries = std::move(v);
    }
    for (auto& [h, v] : pending_enums) {
        auto it = aggr_by_hash_.find(h);
        if (it == aggr_by_hash_.end()) continue;
        auto& a = aggregates_[it->second];
        if (a.enum_entries.empty()) a.enum_entries = std::move(v);
    }
    // Attach class header data (base_hash) when own_hash matches.
    for (const auto& ch : class_headers) {
        auto it = aggr_by_hash_.find(ch.own_hash);
        if (it == aggr_by_hash_.end()) continue;
        auto& a = aggregates_[it->second];
        // Only treat as a base ref if it matches another local
        // aggregate's own_hash. The "no explicit base" sentinel
        // (e.g. TShape's `12 00`) won't match.
        if (ch.base_hash != 0
            && aggr_by_hash_.count(ch.base_hash) != 0) {
            a.base_hash = ch.base_hash;
        }
    }

    // Build a set of "is class header" hashes -- if a 0x2a
    // aggregate's own_hash appears here, it's a class (the
    // `47 00 10 00 00 <own_hash>` header is class-exclusive;
    // records don't carry a vtable so they don't emit one).
    // This is the only RELIABLE way to discriminate record from
    // class -- field-storage byte encodes visibility, not kind
    // (TBag with public fields has storage 0x02 just like a
    // record's members).
    // Classification:
    //   Skip aggregates whose kind is already set by X.3
    //   (low-byte-chained classes).  Without this guard the
    //   classifier below would overwrite Class with Unknown
    //   because own_hash isn't in class_hashes -- only the
    //   chained cls_own is.
    //   - has enum_entries                          -> Enum
    //   - own_hash in class_hashes                  -> Class
    //   - has fields with storage 0xa0              -> PackedRecord
    //   - has fields (default)                      -> Record
    //   - no fields, no enum entries, no class hdr  -> Unknown
    //                                                  (Set candidate)
    for (auto& a : aggregates_) {
        if (a.kind == AggregateKind::Class) continue;  // pinned by X.3
        if (!a.enum_entries.empty()) {
            a.kind = AggregateKind::Enum;
            continue;
        }
        if (class_hashes.count(a.own_hash)
            || (a.linked_hash != 0
                && class_hashes.count(a.linked_hash))) {
            a.kind = AggregateKind::Class;
            continue;
        }
        bool any_packed = false;
        for (const auto& fe : a.fields) {
            if (fe.storage == 0xa0) { any_packed = true; break; }
        }
        if (any_packed)            a.kind = AggregateKind::PackedRecord;
        else if (!a.fields.empty()) a.kind = AggregateKind::Record;
        else                       a.kind = AggregateKind::Unknown;
    }

    // Best-effort total_size: sum of largest (offset + assumed
    // size). For packed we trust the offset+marker_size; for
    // others we just take last_offset + 8 as a conservative
    // approximation (downstream code will tighten this when
    // it actually emits TPI records and knows primitive widths).
    // This is just informational for now.
    for (auto& a : aggregates_) {
        std::uint32_t hi = 0;
        for (const auto& fe : a.fields) {
            if (fe.offset > hi) hi = fe.offset;
        }
        a.total_size = (hi == 0 && a.fields.empty()) ? 0 : hi + 8;
    }

    // Stats
    std::size_t rec=0, packed=0, cls=0, enm=0, unk=0;
    std::size_t fields_total = 0, entries_total = 0;
    for (const auto& a : aggregates_) {
        switch (a.kind) {
          case AggregateKind::Record:        ++rec; break;
          case AggregateKind::PackedRecord:  ++packed; break;
          case AggregateKind::Class:         ++cls; break;
          case AggregateKind::Enum:          ++enm; break;
          default:                           ++unk; break;
        }
        fields_total  += a.fields.size();
        entries_total += a.enum_entries.size();
    }
    std::fprintf(stderr,
                 "[rsm] aggregates: %zu total (records %zu, packed %zu, "
                 "classes %zu, enums %zu, unknown %zu); %zu fields, "
                 "%zu enum entries\n",
                 aggregates_.size(), rec, packed, cls, enm, unk,
                 fields_total, entries_total);
}

// -- Phase B.2 + B.3: aggregate-based type resolution -------------------
//
// Non-primitive variables / params / locals carry their type's own_hash
// in inline_type_id (see Variable::inline_type_id). Walk every variable
// + every proc's params / locals and stamp `pascal_type` from the
// aggregate registry. Per-unit lookup first (Phase B.4) so an RTL
// hash collision cannot mis-route to the wrong type; falls back to
// global last-wins when no enclosing unit anchor is known.
void Reader::resolveAggregateTypes() {
    auto resolveAggrFor = [&](Variable& v) -> bool {
        if (v.is_primitive) return false;
        if (v.inline_type_id == 0) return false;
        if (!v.pascal_type.empty()) return false;
        const std::uint64_t ua = unitAnchorFor(v.file_offset);
        const AggregateType* a = nullptr;
        if (ua != 0) {
            a = findAggregateInUnit(ua, v.inline_type_id);
        }
        if (a == nullptr) {
            a = findAggregateByHash(v.inline_type_id);
        }
        if (a == nullptr) return false;
        v.pascal_type = a->name;
        return true;
    };

    std::size_t typed_globals = 0;
    std::size_t typed_params  = 0;
    std::size_t typed_locals  = 0;
    for (auto& v : variables_) {
        if (resolveAggrFor(v)) ++typed_globals;
    }
    for (auto& proc : procedures_) {
        for (auto& v : proc.params) {
            if (resolveAggrFor(v)) ++typed_params;
        }
        for (auto& v : proc.locals) {
            if (resolveAggrFor(v)) ++typed_locals;
        }
    }
    std::fprintf(stderr,
                 "[rsm] aggregate-typed: %zu globals, %zu params, "
                 "%zu locals\n",
                 typed_globals, typed_params, typed_locals);
}

} // namespace rsm2pdb::rsm
