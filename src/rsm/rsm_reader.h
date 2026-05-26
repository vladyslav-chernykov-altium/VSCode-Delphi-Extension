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
    const std::string&                  error()      const { return error_; }

    // Lookup a primitive by its Pascal name (case-sensitive). Returns
    // nullptr when no such primitive was located in the RSM type table.
    const Primitive* findPrimitive(const std::string& name) const;

    // Lookup a variable by its absolute VA (the .map cross-reference key).
    const Variable* findVariableAt(std::uint64_t address) const;

    // Lookup a procedure record by its function entry VA.
    const ProcedureRecord* findProcedureAt(std::uint64_t address) const;

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
