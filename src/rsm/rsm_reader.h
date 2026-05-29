#pragma once

#include "model/model.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rsm2pdb::rsm {

// Modern Delphi .rsm (Remote Symbol) debug-info file.
//
// Magic is ASCII "CSH7" (not legacy Borland TDS/TD32 "FB09"/"FB0A").
// See docs/02-rsm-format-notes.md for the field-by-field rationale.
//
// This reader currently decodes only the fixed header (bytes 0x00..0x1F
// plus the null-terminated EXE-path string starting at 0x20). The
// metadata stream that begins at offset 0x426 (search paths, type
// table, user-code records) is not yet parsed — see milestone M2-B-lite
// in todo.txt.

// A primitive (built-in) type record parsed from the RSM type table.
// Records are tagged with 0x2A, terminated by 0xFF; the table sits in
// the first few KB of the metadata stream. Size + kind are derived
// from the Pascal name (Delphi's primitive set is closed/well-known);
// raw_type_id is captured for later cross-referencing with variable
// records, but its encoding is not yet fully understood.
struct Primitive {
    std::string         name;          // Pascal name ("Integer", "Byte", ...)
    model::PrimitiveKind kind;
    std::uint16_t       byte_size;
    std::uint16_t       raw_type_id;   // 2 bytes following the 0x9C 0x13 marker
    std::uint64_t       file_offset;   // start of this record (record tag byte)
};

// A global-variable record located inside the RSM. Records are tagged
// with 0x20 and live in the user-code region near the end of the file.
// Layout (validated against fixtures):
//
//   0x20 <namelen u8> <pascal_name> 0x66 0x00 0x00 <payload>
//
//   payload, three variants:
//     <type_marker u8 != 0x9C> <shifted_va u32>            (primitive, plain)
//         used by RTL globals; 5-byte payload, no trailer.
//
//     <type_marker u8> <shifted_va u32>
//         0x9C 0x09 <hash u16> <trailer_type_id u16> 0xFF  (primitive, extended)
//         used by user-code globals; 12-byte payload.
//
//     <inline_type_id u16-LE> <shifted_va u32>             (non-primitive)
//         used when the type is a record / enum / class; 6-byte payload,
//         no trailer. `inline_type_id` indexes into the per-unit type
//         table (not the primitive table).
//
// VA encoding: `actual_va = stored_u32 >> 4`.
//
// Primitive vs non-primitive is discriminated by whether the byte at
// payload[5..6] is the trailer signature `0x9C 0x09` (primitive ext.)
// or a plausible next-record tag (primitive plain). Non-primitive form
// is selected when the first u16 is too large to be a primitive marker
// (markers are small even values 0x02, 0x04, ...).
//
// `type_marker` is a per-unit local id: variables of the same Pascal
// primitive type within one unit share the same marker, but markers
// are not globally stable across units (in unit A, marker 0x02 might
// be Integer; in unit B where Byte is declared first, 0x02 might be
// Byte). Resolution to a concrete Pascal type happens by joining the
// marker with the byte-size derived from the .map next-symbol gap.
struct Variable {
    std::string   name;
    std::uint64_t address;            // decoded absolute VA; or 0 for params/locals
    std::int32_t  stack_offset;       // signed byte offset; only meaningful inside a procedure
    std::uint16_t inline_type_id;     // 0 unless non-primitive
    std::uint16_t trailer_type_id;    // 0 unless primitive-extended; per-unit id
    std::uint8_t  type_marker;        // 0 unless primitive; per-unit type discriminator
    bool          is_primitive;
    bool          has_trailer;        // true for the 12-byte primitive-extended form
    std::uint64_t file_offset;
    // Pascal type name resolved through the variable's enclosing unit's
    // primary type table. Empty string if the resolver couldn't find a
    // matching table entry (e.g. RTL globals whose marker indexes a
    // table we don't decode). When non-empty this is the canonical
    // Pascal name -- "Integer", "Cardinal", "Single", "Boolean", "Char",
    // "string" / "UnicodeString", etc. -- and lookupPrimitiveDesc()
    // resolves it to {kind, byte_size}.
    std::string   pascal_type;
    // The closest-prior unit anchor's file_offset. Stamped after
    // pass 1 so consumers (compose / PDB pipeline) can look up the
    // variable's aggregate scope without re-running upper_bound.
    // 0 means "no enclosing anchor" (rare; matches the same
    // sentinel AggregateType uses).
    std::uint64_t unit_anchor_offset = 0;
};

// A function / procedure record (tag 0x28) with its parameter and
// local-variable sub-records (tags 0x21 / 0x20) decoded.
//
// Layout per rsm-format.txt (M2 phase A entry):
//   0x28 <namelen> <name>
//   0xA0 0x00 0x00                    -- function-flavour sub-tag
//   <hash u32> <shifted_va u32>       -- VA decodes as actual = stored >> 4
//   <u16 X> <0x04 ret_type_marker_u16>
//   sub-records ...
//   0x63                              -- record-end marker
//
// Param sub-record:  0x21 <namelen> <name> <sub_tag_triple>
//                    <type_marker u8> <stack_offset i8>
// Local sub-record:  0x20 <namelen> <name> <sub_tag_triple>
//                    <type_marker u8> <stack_offset i8>
// Sub-tag triple:    0x66 0x00 0x00 for primitive types, 0x62 0x00 0x00 for
//                    non-primitive (record / class / enum).
struct ProcedureRecord {
    std::string             name;          // unqualified Pascal name
    std::uint64_t           address;       // decoded VA
    std::vector<Variable>   params;
    std::vector<Variable>   locals;        // includes Result for functions
    std::uint64_t           file_offset;
    std::uint64_t           file_offset_end;  // one past the 0x63 end marker
    // True for Pascal `nested functions` (a `function inside another
    // function`). They receive an implicit `static link` -- a pointer
    // to the enclosing function's stack frame -- in rcx, spilled to
    // [rbp+sub_rsp+16] like a real first parameter would be. The
    // sub-records below do NOT include this slot; consumers need to
    // synthesise a `$frame_outer` local themselves.
    bool                    has_static_link = false;
};

// One member of a record / class type, parsed from a 0x2c sub-record.
//
// Encoding (see rsm-format.txt 2026-05-28 Phase A entries):
//   2c <namelen> <name>
//      00 <storage u8> 00           -- storage: 0x00 private, 0x02 public,
//                                     0x04 protected, 0xa0 packed-record
//      <type-ref>                   -- 1 byte primitive marker
//                                     OR 2-byte own_hash of another
//                                     type record in the same unit.
//      <offset 1B or 2B>            -- offset within parent; 2-byte form
//                                     marked by LSB=1, decoded as
//                                     `real = (u16-1) / 4`.
//      <internals 13B or 8B>        -- file back-refs, ignored.
//      <parent_hash u16 LE>
//      0xff                         -- field terminator.
//
// `type_hash` is set when the field's type is another aggregate;
// `primitive_marker` is set otherwise. Exactly one of them is
// non-zero (we never see both empty -- types that fail to resolve
// fall back to a synthetic byte[N] downstream).
struct FieldEntry {
    std::string   name;
    std::uint32_t offset = 0;        // real byte offset within parent
    std::uint8_t  primitive_marker = 0;  // small even from kPrimitiveTable
    std::uint16_t type_hash = 0;     // own_hash of another aggregate
    std::uint8_t  storage = 0x02;    // raw byte; visibility / packed flag
};

// One enumerator in a Pascal enum type, parsed from a 0x25 sub-record.
struct EnumEntry {
    std::string  name;
    std::int64_t ordinal = 0;        // decoded value (e.g. clRed=0, clGreen=1)
};

// Kind discriminator for AggregateType. See classify() in
// rsm_reader.cpp for the inference rules (we don't trust the 0x2a
// record's `kind` byte alone -- field-storage and entry presence
// disambiguate).
enum class AggregateKind : std::uint8_t {
    Unknown = 0,
    Record,                          // simple `record ... end`
    PackedRecord,                    // `packed record ... end`
    Class,                           // `class ... end`
    Enum,                            // `(a, b, c)`
    Set,                             // `set of TColor` (best-effort)
};

// A user-declared Pascal aggregate type, parsed from a 0x2a record
// + its trailing 0x2c / 0x25 sub-records (which live in a separate
// metadata cluster but are linked by parent_hash). The own_hash is
// the unit-local identifier that 0x66-form sub-records use to point
// at this type.
struct AggregateType {
    std::string             name;            // Pascal name ("TPoint", ...)
    std::uint16_t           own_hash = 0;    // unique within this unit
    AggregateKind           kind = AggregateKind::Unknown;
    std::vector<FieldEntry> fields;          // records / classes
    std::vector<EnumEntry>  enum_entries;    // enums only
    std::uint16_t           base_hash = 0;   // classes only: parent's own_hash
    // Secondary hash carried by 0x2a records whose kind byte has
    // bit 0x80 set (AdvPCB-style large-project classes -- the type
    // record at own_hash carries the NAME, while the actual class
    // header + fields live under linked_hash). Empirically 81.8%
    // of `kind & 0x80` 0x2a records in AdvPCB.rsm have linked_hash
    // that exactly matches a 0x47 class-header own_hash. We index
    // each such aggregate under BOTH own_hash and linked_hash so a
    // variable referencing either resolves to the same record.
    std::uint16_t           linked_hash = 0;
    std::uint32_t           total_size = 0;  // bytes; 0 if unknown
    std::uint64_t           file_offset = 0; // 0x2a record start (debug aid)
    // The closest-prior unit anchor's file offset. Pins this
    // aggregate to a specific unit's local hash space -- crucial on
    // multi-unit projects where RTL units can declare distinct types
    // sharing the same own_hash value as a user-unit aggregate.
    // 0 means "no enclosing unit anchor found" (rare; falls back to
    // global last-wins lookup).
    std::uint64_t           unit_anchor_offset = 0;
};

struct Header {
    static constexpr std::uint32_t kMagic = 0x37485343;  // "CSH7" LE
    static constexpr std::uint32_t kMetadataStart = 0x00000420;
    static constexpr std::uint32_t kFlagsConst    = 0x2000234D;
    static constexpr std::uint32_t kLegacyImageBase = 0x00400000;

    std::uint32_t magic            = 0;  // 0x00  "CSH7"
    std::uint32_t metadata_start   = 0;  // 0x04  always 0x420 in our samples
    std::uint32_t unit_count       = 0;  // 0x08  number of linked CUs
    std::uint32_t version_minor    = 0;  // 0x0C  always 1 in our samples
    std::uint32_t timestamp        = 0;  // 0x10  Delphi compiler-build epoch
    std::uint32_t flags            = 0;  // 0x14  always 0x2000234D
    std::uint32_t legacy_imagebase = 0;  // 0x18  always 0x00400000
    std::uint32_t reserved_1c      = 0;  // 0x1C  zero in our samples
    std::string   exe_path;              // 0x20  null-terminated ASCII
};

class Reader {
public:
    bool open(const std::string& path);

    const Header&                       header()     const { return header_; }
    const std::vector<Primitive>&       primitives() const { return primitives_; }
    const std::vector<Variable>&        variables()  const { return variables_; }
    const std::vector<ProcedureRecord>& procedures() const { return procedures_; }
    const std::vector<AggregateType>&   aggregates() const { return aggregates_; }
    const std::string&                  error()      const { return error_; }

    // Lookup a primitive by its Pascal name (case-sensitive). Returns
    // nullptr when no such primitive was located in the RSM type table.
    const Primitive* findPrimitive(const std::string& name) const;

    // Resolve a Pascal type name to its (kind, byte width) pair using
    // the built-in Win64 primitive table. Returns nullopt for unknown
    // names. Accepts canonical Delphi names ("Integer", "Cardinal",
    // "Single", "Boolean", "Char", "UnicodeString", etc.) -- the same
    // strings that Variable::pascal_type carries after open().
    struct ResolvedPrimitive {
        model::PrimitiveKind kind;
        std::uint16_t        byte_size;
    };
    static std::optional<ResolvedPrimitive>
        resolvePrimitive(std::string_view pascal_name);

    // Lookup a variable by its absolute VA (the .map cross-reference key).
    const Variable* findVariableAt(std::uint64_t address) const;

    // Lookup a procedure record by its function entry VA.
    const ProcedureRecord* findProcedureAt(std::uint64_t address) const;

    // Lookup an aggregate type by its unit-local own_hash. own_hash
    // is per-unit, NOT globally unique -- last-wins is used: the
    // aggregate added last to the list (file-order) for a given
    // hash is returned. Prefer findAggregateInUnit() when the caller
    // knows which unit the lookup originates from.
    const AggregateType* findAggregateByHash(std::uint16_t hash) const;

    // Lookup an aggregate by (unit, hash). `unit_anchor_offset` is
    // the file offset of the unit's anchor record -- callers can get
    // it by feeding a Variable's file_offset to unitAnchorFor().
    // Returns nullptr if no aggregate matches that exact (unit, hash)
    // pair (e.g. when the hash actually belongs to a different unit).
    const AggregateType* findAggregateInUnit(std::uint64_t unit_anchor_offset,
                                             std::uint16_t hash) const;

    // Returns the file offset of the unit anchor that contains the
    // given file_offset, or 0 if none. Use this with
    // findAggregateInUnit() to scope a hash lookup to a variable's
    // own unit.
    std::uint64_t unitAnchorFor(std::uint64_t file_offset) const;

    // Look up the Pascal type name a per-unit primitive marker
    // resolves to. Used by aggregate-field decoration: a field's
    // primitive_marker is a position in its enclosing unit's
    // 0x66 type-table run. Returns nullptr if either the unit
    // anchor or the marker is unknown.
    const std::string* primitiveNameForMarker(
        std::uint64_t unit_anchor_offset,
        std::uint8_t marker) const;

    void dump(std::FILE* out) const;

private:
    std::string                  path_;
    Header                       header_;
    std::vector<Primitive>       primitives_;
    std::vector<Variable>        variables_;
    std::vector<ProcedureRecord> procedures_;
    std::string                  error_;
    // VA -> index into variables_ / procedures_, built once after scanning.
    // Speeds up findVariableAt / findProcedureAt from O(N) to O(1).
    std::unordered_map<std::uint64_t, std::size_t> var_by_va_;
    std::unordered_map<std::uint64_t, std::size_t> proc_by_va_;
    // Aggregates (records / classes / enums / sets). Indexed two
    // ways: aggr_by_hash_ is global last-wins (for callers without
    // a unit context), aggr_by_unit_hash_ is per-unit precise (for
    // callers that have a Variable's file_offset to lift to a unit
    // anchor first). See findAggregateInUnit() / unitAnchorFor().
    std::vector<AggregateType>                     aggregates_;
    std::unordered_map<std::uint16_t, std::size_t> aggr_by_hash_;
    struct UnitHashKey {
        std::uint64_t unit_anchor_offset;
        std::uint16_t hash;
        bool operator==(const UnitHashKey& o) const {
            return unit_anchor_offset == o.unit_anchor_offset
                && hash == o.hash;
        }
    };
    struct UnitHashKeyHasher {
        std::size_t operator()(const UnitHashKey& k) const noexcept {
            // 64-bit FNV-style mix.
            std::uint64_t h = k.unit_anchor_offset;
            h ^= (static_cast<std::uint64_t>(k.hash) * 0x9E3779B97F4A7C15ull);
            h ^= (h >> 33);
            h *= 0xC2B2AE3D27D4EB4Full;
            return static_cast<std::size_t>(h);
        }
    };
    std::unordered_map<UnitHashKey, std::size_t,
                       UnitHashKeyHasher>          aggr_by_unit_hash_;
    // Unit-anchor offsets in ascending file order, for upper_bound
    // queries via unitAnchorFor(). Populated alongside the existing
    // primary-type-table scan but kept as a member here so the
    // aggregate parser + downstream callers can both use it.
    std::vector<std::uint64_t>                     unit_anchor_offsets_;
    // Per-unit primary type-table name list, keyed by the unit's
    // anchor offset. Position N within the list is the Pascal type
    // name for marker = 2 * (N+1). Used by aggregate-field decoration
    // to resolve a field's primitive_marker to a CodeView TypeKind.
    std::unordered_map<std::uint64_t,
                       std::vector<std::string>>   primary_table_by_anchor_;
};

// Decorate a populated model::Module with type information derived
// from the RSM. For each variable Symbol in the module we look up the
// matching RSM Variable by absolute VA:
//
//   - non-primitive Variables get a `byte[N]` ArrayType where N is
//     the next-symbol-gap derived from the module's other variable
//     addresses.
//
//   - primitive Variables are grouped by their per-unit `type_marker`.
//     Within a CU, all same-marker variables share the same Pascal
//     primitive type; the type is inferred from the minimum gap
//     observed across the group (1 -> Byte, 2 -> Word, 4 -> Integer,
//     8 -> Int64).
//
// Symbols whose VA doesn't match any RSM record are left at kNoType
// and the DWARF emitter will fall back to the untyped variable shape.
void decorateTypes(const Reader& reader, model::Module& mod);

} // namespace rsm2pdb::rsm
