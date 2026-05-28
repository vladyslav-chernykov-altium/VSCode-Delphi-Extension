// DWARF v5 emitter, milestone D1.
//
// Emits:
//   .debug_info   - one DW_TAG_compile_unit per CompileUnit.
//                   Attributes: producer, language, name, comp_dir,
//                   low_pc, high_pc, stmt_list.
//                   No DW_TAG_subprogram yet (D2). No types yet (D3).
//   .debug_abbrev - one shared abbreviation for the CU shape.
//   .debug_line   - DWARF v5 line program per CU.
//   .debug_str    - empty for D1 (we inline strings via DW_FORM_string).
//   .debug_line_str - empty for D1 (same).
//
// Verified against llvm::DWARFContext in the unit tests, then against
// gdb after PE injection.

#include "dwarf/dwarf_emitter.h"

#include "llvm/BinaryFormat/Dwarf.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace rsm2pdb::dwarf {

namespace {

// ---------------------------------------------------------------------
// Byte buffer with little-endian writes and LEB128 encoding.
// ---------------------------------------------------------------------
class BytesBuf {
public:
    void u8(std::uint8_t v)  { bytes_.push_back(v); }
    void u16(std::uint16_t v) {
        bytes_.push_back(static_cast<std::uint8_t>(v));
        bytes_.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<std::uint8_t>(v >> (i*8)));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<std::uint8_t>(v >> (i*8)));
    }
    void s8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }

    void uleb128(std::uint64_t v) {
        do {
            std::uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            bytes_.push_back(b);
        } while (v);
    }
    void sleb128(std::int64_t v) {
        bool more = true;
        while (more) {
            std::uint8_t b = v & 0x7F;
            v >>= 7;  // arithmetic shift
            bool sign = (b & 0x40) != 0;
            if ((v == 0 && !sign) || (v == -1 && sign)) {
                more = false;
            } else {
                b |= 0x80;
            }
            bytes_.push_back(b);
        }
    }

    // Null-terminated string.
    void cstr(const std::string& s) {
        bytes_.insert(bytes_.end(), s.begin(), s.end());
        bytes_.push_back(0);
    }

    void raw(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + n);
    }

    // Reserve a fixed-size placeholder; returns its offset so the caller
    // can patch later (used for 4-byte unit_length fields).
    std::size_t reservedU32() {
        std::size_t off = bytes_.size();
        bytes_.insert(bytes_.end(), 4, 0);
        return off;
    }
    void patchU32(std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            bytes_[off + i] = static_cast<std::uint8_t>(v >> (i*8));
    }

    std::size_t size() const { return bytes_.size(); }
    std::vector<std::uint8_t>&& take() && { return std::move(bytes_); }
    const std::vector<std::uint8_t>& view() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// ---------------------------------------------------------------------
// Split a path into (directory, file). Both relative-friendly.
// ---------------------------------------------------------------------
struct PathSplit { std::string dir, file; };
PathSplit splitPath(const std::string& path) {
    auto slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return {"", path};
    return {path.substr(0, slash), path.substr(slash + 1)};
}

// ---------------------------------------------------------------------
// Abbreviation codes.
// ---------------------------------------------------------------------
constexpr std::uint64_t kAbbrevCu               = 1;
constexpr std::uint64_t kAbbrevSubprogram       = 2;
constexpr std::uint64_t kAbbrevVariable         = 3;
constexpr std::uint64_t kAbbrevTypedVariable    = 4;
constexpr std::uint64_t kAbbrevBaseType         = 5;
constexpr std::uint64_t kAbbrevArrayType        = 6;
constexpr std::uint64_t kAbbrevSubrangeType     = 7;
constexpr std::uint64_t kAbbrevSubprogramWithKids = 8;   // children + frame_base
constexpr std::uint64_t kAbbrevFormalParameter  = 9;
constexpr std::uint64_t kAbbrevLocalVariable    = 10;

void writeAbbrevTable(BytesBuf& abbrev) {
    using namespace llvm::dwarf;

    // Abbrev 1: DW_TAG_compile_unit, HAS children.
    abbrev.uleb128(kAbbrevCu);
    abbrev.uleb128(DW_TAG_compile_unit);
    abbrev.u8(DW_CHILDREN_yes);
    abbrev.uleb128(DW_AT_producer);  abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_language);  abbrev.uleb128(DW_FORM_data2);
    abbrev.uleb128(DW_AT_name);      abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_comp_dir);  abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_low_pc);    abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_high_pc);   abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_stmt_list); abbrev.uleb128(DW_FORM_sec_offset);
    abbrev.uleb128(0);               abbrev.uleb128(0);

    // Abbrev 2: DW_TAG_subprogram, no children.
    abbrev.uleb128(kAbbrevSubprogram);
    abbrev.uleb128(DW_TAG_subprogram);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_name);      abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_low_pc);    abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_high_pc);   abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_external);  abbrev.uleb128(DW_FORM_flag);
    abbrev.uleb128(0);               abbrev.uleb128(0);

    // Abbrev 3: DW_TAG_variable, no children, untyped fallback.
    // Used when we couldn't resolve a Pascal type for the variable.
    abbrev.uleb128(kAbbrevVariable);
    abbrev.uleb128(DW_TAG_variable);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_name);      abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_location);  abbrev.uleb128(DW_FORM_exprloc);
    abbrev.uleb128(DW_AT_external);  abbrev.uleb128(DW_FORM_flag);
    abbrev.uleb128(0);               abbrev.uleb128(0);

    // Abbrev 4: DW_TAG_variable with DW_AT_type (ref_addr -> type DIE).
    abbrev.uleb128(kAbbrevTypedVariable);
    abbrev.uleb128(DW_TAG_variable);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_name);      abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_location);  abbrev.uleb128(DW_FORM_exprloc);
    abbrev.uleb128(DW_AT_type);      abbrev.uleb128(DW_FORM_ref_addr);
    abbrev.uleb128(DW_AT_external);  abbrev.uleb128(DW_FORM_flag);
    abbrev.uleb128(0);               abbrev.uleb128(0);

    // Abbrev 5: DW_TAG_base_type, no children.
    abbrev.uleb128(kAbbrevBaseType);
    abbrev.uleb128(DW_TAG_base_type);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_name);       abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_byte_size);  abbrev.uleb128(DW_FORM_data1);
    abbrev.uleb128(DW_AT_encoding);   abbrev.uleb128(DW_FORM_data1);
    abbrev.uleb128(0);                abbrev.uleb128(0);

    // Abbrev 6: DW_TAG_array_type with one subrange child.
    abbrev.uleb128(kAbbrevArrayType);
    abbrev.uleb128(DW_TAG_array_type);
    abbrev.u8(DW_CHILDREN_yes);
    abbrev.uleb128(DW_AT_type);       abbrev.uleb128(DW_FORM_ref_addr);
    abbrev.uleb128(0);                abbrev.uleb128(0);

    // Abbrev 7: DW_TAG_subrange_type, no children.
    abbrev.uleb128(kAbbrevSubrangeType);
    abbrev.uleb128(DW_TAG_subrange_type);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_upper_bound); abbrev.uleb128(DW_FORM_udata);
    abbrev.uleb128(0);                 abbrev.uleb128(0);

    // Abbrev 8: DW_TAG_subprogram WITH children + DW_AT_frame_base.
    // Used when we have params and/or locals to emit as children.
    abbrev.uleb128(kAbbrevSubprogramWithKids);
    abbrev.uleb128(DW_TAG_subprogram);
    abbrev.u8(DW_CHILDREN_yes);
    abbrev.uleb128(DW_AT_name);        abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_low_pc);      abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_high_pc);     abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_external);    abbrev.uleb128(DW_FORM_flag);
    abbrev.uleb128(DW_AT_frame_base);  abbrev.uleb128(DW_FORM_exprloc);
    abbrev.uleb128(0);                 abbrev.uleb128(0);

    // Abbrev 9: DW_TAG_formal_parameter with location + type.
    abbrev.uleb128(kAbbrevFormalParameter);
    abbrev.uleb128(DW_TAG_formal_parameter);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_name);        abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_type);        abbrev.uleb128(DW_FORM_ref_addr);
    abbrev.uleb128(DW_AT_location);    abbrev.uleb128(DW_FORM_exprloc);
    abbrev.uleb128(0);                 abbrev.uleb128(0);

    // Abbrev 10: DW_TAG_variable (local) with location + type, no external.
    abbrev.uleb128(kAbbrevLocalVariable);
    abbrev.uleb128(DW_TAG_variable);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_name);        abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_type);        abbrev.uleb128(DW_FORM_ref_addr);
    abbrev.uleb128(DW_AT_location);    abbrev.uleb128(DW_FORM_exprloc);
    abbrev.uleb128(0);                 abbrev.uleb128(0);

    // End-of-table marker.
    abbrev.uleb128(0);
}

// ---------------------------------------------------------------------
// Line program for one CompileUnit.
//
// DWARF v5 line header:
//   u32 unit_length         (excludes this field)
//   u16 version             (5)
//   u8  address_size        (8 on x64)
//   u8  segment_selector_size (0)
//   u32 header_length       (from after this field to end of headers)
//   u8  minimum_instruction_length
//   u8  maximum_operations_per_instruction
//   u8  default_is_stmt
//   s8  line_base
//   u8  line_range
//   u8  opcode_base
//   u8[opcode_base-1] standard_opcode_lengths
//
//   Directory entry format + directory count + entries
//   File   entry format + file count    + entries
//
// Returns the byte offset (within .debug_line) where this program
// started - used as DW_AT_stmt_list in the CU DIE.
// ---------------------------------------------------------------------
std::uint32_t writeLineProgram(BytesBuf& line, const model::CompileUnit& cu) {
    using namespace llvm::dwarf;

    const std::uint32_t program_start_offset =
        static_cast<std::uint32_t>(line.size());

    auto split = splitPath(cu.source_path);

    // unit_length placeholder.
    std::size_t unit_length_pos = line.reservedU32();
    std::size_t length_field_end = line.size();   // after unit_length

    line.u16(5);    // version
    line.u8(8);     // address_size
    line.u8(0);     // segment_selector_size

    // header_length placeholder.
    std::size_t header_length_pos = line.reservedU32();
    std::size_t header_length_start = line.size();

    line.u8(1);     // minimum_instruction_length
    line.u8(1);     // maximum_operations_per_instruction
    line.u8(1);     // default_is_stmt
    line.s8(-5);    // line_base
    line.u8(14);    // line_range
    line.u8(13);    // opcode_base (DWARF v5 standard)

    // standard_opcode_lengths[1..12] (one less than opcode_base).
    // Values per DWARF spec for the 12 standard opcodes.
    static const std::uint8_t kStdOpLens[12] = {
        0, // DW_LNS_copy
        1, // DW_LNS_advance_pc
        1, // DW_LNS_advance_line
        1, // DW_LNS_set_file
        1, // DW_LNS_set_column
        0, // DW_LNS_negate_stmt
        0, // DW_LNS_set_basic_block
        0, // DW_LNS_const_add_pc
        1, // DW_LNS_fixed_advance_pc
        0, // DW_LNS_set_prologue_end
        0, // DW_LNS_set_epilogue_begin
        1  // DW_LNS_set_isa
    };
    for (auto v : kStdOpLens) line.u8(v);

    // ---- Directory table (DWARF v5 format) ----
    line.u8(1);                       // directory_entry_format_count
    line.uleb128(DW_LNCT_path);
    line.uleb128(DW_FORM_string);
    line.uleb128(1);                  // directories_count
    line.cstr(split.dir);             // single entry: the compilation dir

    // ---- File name table (DWARF v5 format) ----
    // Entry 0 corresponds to the compilation unit's primary source.
    line.u8(2);                       // file_name_entry_format_count
    line.uleb128(DW_LNCT_path);
    line.uleb128(DW_FORM_string);
    line.uleb128(DW_LNCT_directory_index);
    line.uleb128(DW_FORM_data1);
    line.uleb128(1);                  // file_names_count
    line.cstr(split.file);            // path
    line.u8(0);                       // directory_index = 0

    // ---- End of header; patch header_length ----
    line.patchU32(header_length_pos,
                  static_cast<std::uint32_t>(line.size() - header_length_start));

    // ---- Line number program ----
    // State machine: address=0, file=1, line=1, column=0, is_stmt=default,
    // basic_block=false, end_sequence=false.
    //
    // We emit, for each LineEntry:
    //   DW_LNE_set_address  (initial only; thereafter DW_LNS_advance_pc)
    //   DW_LNS_advance_line / advance_pc as needed
    //   DW_LNS_copy
    // Then at end:
    //   DW_LNE_end_sequence

    // State machine: address=0, file=1 (per spec), line=1.
    // We have only one file (index 0), so we must explicitly set file=0
    // before the first copy. Without this, rows reference file 1 and
    // are silently dropped by readers.
    line.u8(DW_LNS_set_file);
    line.uleb128(0);

    std::uint64_t state_address = 0;
    std::uint32_t state_line    = 1;
    bool          have_initial_addr = false;

    auto emitSetAddress = [&](std::uint64_t addr) {
        // Extended opcode: DW_LNE_set_address takes an address-sized operand.
        line.u8(0);                   // extended opcode prefix
        line.uleb128(1 + 8);          // length = 1 (sub-opcode) + 8 (addr)
        line.u8(DW_LNE_set_address);
        line.u64(addr);
        state_address = addr;
        have_initial_addr = true;
    };

    auto emitAdvanceLine = [&](std::int64_t delta) {
        if (delta == 0) return;
        line.u8(DW_LNS_advance_line);
        line.sleb128(delta);
    };

    auto emitAdvancePc = [&](std::uint64_t delta) {
        if (delta == 0) return;
        line.u8(DW_LNS_advance_pc);
        line.uleb128(delta);
    };

    auto emitCopy = [&]() {
        line.u8(DW_LNS_copy);
    };

    // Sort line entries by address (.map is usually in order but defensive).
    std::vector<model::LineEntry> sorted = cu.lines;
    std::sort(sorted.begin(), sorted.end(),
              [](const model::LineEntry& a, const model::LineEntry& b) {
                  return a.address < b.address;
              });

    for (const auto& le : sorted) {
        if (!have_initial_addr) {
            emitSetAddress(le.address);
        } else if (le.address != state_address) {
            emitAdvancePc(le.address - state_address);
            state_address = le.address;
        }
        if (static_cast<std::uint32_t>(static_cast<std::int64_t>(le.line)) != state_line) {
            std::int64_t delta = static_cast<std::int64_t>(le.line) -
                                 static_cast<std::int64_t>(state_line);
            emitAdvanceLine(delta);
            state_line = le.line;
        }
        emitCopy();
    }

    // End the sequence.
    line.u8(0);
    line.uleb128(1);
    line.u8(DW_LNE_end_sequence);

    // ---- Patch unit_length ----
    std::size_t total_after_length = line.size() - length_field_end;
    line.patchU32(unit_length_pos, static_cast<std::uint32_t>(total_after_length));

    return program_start_offset;
}

// ---------------------------------------------------------------------
// Compute (low, high) address range covered by a CU.
//
// Includes BOTH line-entry addresses AND function-symbol addresses.
// Why both:
//   - Line entries cover the source-mapped instructions.
//   - Function symbols cover whole-function ranges, including
//     compiler-generated entries (e.g. Delphi's <Unit>.<Unit> unit-init
//     for the .dpr's `begin..end` block) that may have only sparse or
//     missing line entries.
// gdb consults a CU's line program only for addresses within
// [low_pc, high_pc). If a function falls outside the line-derived
// range, breaks in that function show as "no source" -- which is the
// exact symptom that map2pdb avoids by emitting per-function ranges
// in CodeView. We get the equivalent by widening the CU range to
// cover every function symbol attributed to this CU.
// ---------------------------------------------------------------------
struct AddressRange { std::uint64_t lo, hi; };
AddressRange cuRange(const model::CompileUnit& cu) {
    AddressRange r{0, 0};
    bool first = true;
    auto absorb = [&](std::uint64_t a) {
        if (a == 0) return;
        if (first) { r.lo = r.hi = a; first = false; }
        else {
            if (a < r.lo) r.lo = a;
            if (a > r.hi) r.hi = a;
        }
    };
    for (const auto& le : cu.lines) absorb(le.address);
    for (const auto& sym : cu.symbols) {
        if (sym.kind == model::SymbolKind::Function) absorb(sym.address);
    }
    return r;
}

// ---------------------------------------------------------------------
// Compute high_pc for a symbol: use the next-higher symbol's address
// within the same CU, falling back to the CU's high_pc.
// ---------------------------------------------------------------------
std::uint64_t symbolEnd(const model::CompileUnit& cu,
                        const model::Symbol& sym,
                        std::uint64_t cu_high) {
    std::uint64_t best = cu_high;
    for (const auto& other : cu.symbols) {
        if (other.address > sym.address && other.address < best) {
            best = other.address;
        }
    }
    return best > sym.address ? best : sym.address + 1;
}

// ---------------------------------------------------------------------
// Pascal-friendly display name for a PrimitiveKind. Pairs with the
// DWARF encoding so debuggers render values correctly.
// ---------------------------------------------------------------------
struct PrimitiveDescriptor {
    const char*   name;
    std::uint8_t  byte_size;
    std::uint8_t  encoding;   // llvm::dwarf::DW_ATE_*
};
PrimitiveDescriptor describePrimitive(model::PrimitiveKind k) {
    using namespace llvm::dwarf;
    switch (k) {
    case model::PrimitiveKind::Bool:    return {"Boolean", 1, DW_ATE_boolean};
    case model::PrimitiveKind::Char:    return {"AnsiChar", 1, DW_ATE_unsigned_char};
    case model::PrimitiveKind::WChar:   return {"Char", 2, DW_ATE_UTF};
    case model::PrimitiveKind::Int8:    return {"ShortInt", 1, DW_ATE_signed};
    case model::PrimitiveKind::Int16:   return {"SmallInt", 2, DW_ATE_signed};
    case model::PrimitiveKind::Int32:   return {"Integer",  4, DW_ATE_signed};
    case model::PrimitiveKind::Int64:   return {"Int64",    8, DW_ATE_signed};
    case model::PrimitiveKind::UInt8:   return {"Byte",     1, DW_ATE_unsigned};
    case model::PrimitiveKind::UInt16:  return {"Word",     2, DW_ATE_unsigned};
    case model::PrimitiveKind::UInt32:  return {"Cardinal", 4, DW_ATE_unsigned};
    case model::PrimitiveKind::UInt64:  return {"UInt64",   8, DW_ATE_unsigned};
    case model::PrimitiveKind::Float32: return {"Single",   4, DW_ATE_float};
    case model::PrimitiveKind::Float64: return {"Double",   8, DW_ATE_float};
    case model::PrimitiveKind::Float80: return {"Extended", 10, DW_ATE_float};
    // String-pointer primitives. DWARF has no built-in
    // pointer-to-char "string display" flag, so we describe them as
    // 8-byte unsigned addresses (the gdb pretty-printer can still
    // dereference manually). The PDB backend renders them as native
    // CodeView pointer-to-char simple types where cdb auto-displays
    // them as `"..."`.
    case model::PrimitiveKind::PChar:   return {"PAnsiChar", 8, DW_ATE_address};
    case model::PrimitiveKind::PWChar:  return {"PWideChar", 8, DW_ATE_address};
    }
    return {"unknown", 1, DW_ATE_unsigned};
}

// ---------------------------------------------------------------------
// Emit a single type DIE; returns the absolute byte offset within
// .debug_info where the DIE begins (used for DW_AT_type refs).
// ---------------------------------------------------------------------
std::uint32_t emitBaseTypeDIE(BytesBuf& info, model::PrimitiveKind k) {
    const auto off = static_cast<std::uint32_t>(info.size());
    const auto desc = describePrimitive(k);
    info.uleb128(kAbbrevBaseType);
    info.cstr(desc.name);
    info.u8(desc.byte_size);
    info.u8(desc.encoding);
    return off;
}

std::uint32_t emitArrayTypeDIE(BytesBuf& info,
                               std::uint32_t element_offset,
                               std::uint64_t length) {
    const auto off = static_cast<std::uint32_t>(info.size());
    info.uleb128(kAbbrevArrayType);
    info.u32(element_offset);
    // Single subrange child. gdb's Pascal mode defaults the lower
    // bound to 1, so emit upper_bound = length to cover all N elements
    // as the Pascal range [1..N].
    info.uleb128(kAbbrevSubrangeType);
    info.uleb128(length);                          // DW_AT_upper_bound
    info.u8(0);                                    // children-end marker
    return off;
}

// ---------------------------------------------------------------------
// Compile-unit DIE for .debug_info.
// ---------------------------------------------------------------------
void writeCompileUnit(BytesBuf& info,
                      const model::Module& mod,
                      const model::CompileUnit& cu,
                      const std::string& producer,
                      std::uint32_t stmt_list_offset,
                      std::uint32_t abbrev_table_offset) {
    using namespace llvm::dwarf;

    // CU header (DWARF v5, 32-bit DWARF):
    //   u32 unit_length     (excluding this field)
    //   u16 version         (5)
    //   u8  unit_type       (DW_UT_compile)
    //   u8  address_size    (8)
    //   u32 debug_abbrev_offset
    std::size_t unit_length_pos = info.reservedU32();
    std::size_t length_field_end = info.size();

    info.u16(5);
    info.u8(DW_UT_compile);
    info.u8(8);
    info.u32(abbrev_table_offset);

    // DIE: abbrev code 1 with attributes in order matching writeAbbrevTable.
    info.uleb128(kAbbrevCu);
    info.cstr(producer);                  // DW_AT_producer        (string)
    info.u16(DW_LANG_Pascal83);           // DW_AT_language        (data2)

    auto split = splitPath(cu.source_path);
    info.cstr(split.file);                // DW_AT_name            (string)
    info.cstr(split.dir);                 // DW_AT_comp_dir        (string)

    auto rng = cuRange(cu);
    info.u64(rng.lo);                     // DW_AT_low_pc          (addr)
    info.u64(rng.hi + 1);                 // DW_AT_high_pc         (addr, exclusive)
    info.u32(stmt_list_offset);           // DW_AT_stmt_list       (sec_offset)

    // ---- Type DIEs first ----
    // Walk this CU's variable symbols and emit unique base-types and
    // array-types up front so subsequent DW_AT_type references can
    // point at them. The offsets recorded here are absolute byte
    // positions within .debug_info (DW_FORM_ref_addr semantics).
    std::map<model::TypeId, std::uint32_t> type_offsets;
    auto ensureType = [&](model::TypeId tid) -> std::uint32_t {
        if (tid == model::kNoType) return 0;
        auto it = type_offsets.find(tid);
        if (it != type_offsets.end()) return it->second;
        const auto& t = mod.getType(tid);
        std::uint32_t off = 0;
        if (std::holds_alternative<model::PrimitiveType>(t.kind)) {
            const auto& p = std::get<model::PrimitiveType>(t.kind);
            off = emitBaseTypeDIE(info, p.kind);
        } else if (std::holds_alternative<model::ArrayType>(t.kind)) {
            const auto& a = std::get<model::ArrayType>(t.kind);
            // Element type was emitted in the prepass below.
            std::uint32_t element_off = 0;
            if (a.element != model::kNoType) {
                auto eit = type_offsets.find(a.element);
                if (eit != type_offsets.end()) element_off = eit->second;
            }
            off = emitArrayTypeDIE(info, element_off, a.length);
        } else {
            // RecordType / EnumType / PointerType: not supported in
            // B-lite. Caller falls back to the untyped abbrev.
            return 0;
        }
        type_offsets[tid] = off;
        return off;
    };

    // Two-pass emission so array elements are always emitted before
    // their containing arrays. Walk variables, params, and locals.
    auto preEnsureArrayElement = [&](model::TypeId tid) {
        if (tid == model::kNoType) return;
        const auto& t = mod.getType(tid);
        if (std::holds_alternative<model::ArrayType>(t.kind)) {
            const auto& a = std::get<model::ArrayType>(t.kind);
            if (a.element != model::kNoType) ensureType(a.element);
        }
    };
    for (const auto& sym : cu.symbols) {
        if (sym.kind == model::SymbolKind::Variable) preEnsureArrayElement(sym.type);
        if (sym.kind == model::SymbolKind::Function) {
            for (const auto& p : sym.params) preEnsureArrayElement(p.type);
            for (const auto& l : sym.locals) preEnsureArrayElement(l.type);
        }
    }
    for (const auto& sym : cu.symbols) {
        if (sym.kind == model::SymbolKind::Variable) ensureType(sym.type);
        if (sym.kind == model::SymbolKind::Function) {
            for (const auto& p : sym.params) ensureType(p.type);
            for (const auto& l : sym.locals) ensureType(l.type);
        }
    }

    // ---- Child DIEs: subprograms (functions) + variables (data) ----
    // Function symbols must fall within the CU's code address range
    // (computed from line entries); variables are in data segments
    // and have no such range constraint - they just belong to the
    // CU because they share the module name.
    // Helper: serialize a signed integer as SLEB128. We need the byte
    // count up front to fill in the exprloc length prefix, so encode
    // into a temporary then splice.
    auto sleb128Bytes = [](std::int64_t v) {
        std::vector<std::uint8_t> out;
        bool more = true;
        while (more) {
            std::uint8_t b = static_cast<std::uint8_t>(v & 0x7F);
            v >>= 7;
            const bool sign = (b & 0x40) != 0;
            if ((v == 0 && !sign) || (v == -1 && sign)) more = false;
            else b |= 0x80;
            out.push_back(b);
        }
        return out;
    };

    for (const auto& sym : cu.symbols) {
        switch (sym.kind) {
        case model::SymbolKind::Function: {
            if (sym.address < rng.lo || sym.address > rng.hi) break;
            std::uint64_t end = symbolEnd(cu, sym, rng.hi + 1);
            const bool has_kids = !sym.params.empty() || !sym.locals.empty();

            if (!has_kids) {
                info.uleb128(kAbbrevSubprogram);
                info.cstr(sym.name);
                info.u64(sym.address);
                info.u64(end);
                info.u8(1);  // external
            } else {
                // Subprogram with children + frame_base.
                //
                // model::LocalVar::stack_offset is the already-resolved
                // signed byte offset from RBP, computed by
                // compose::resolveFunction from the prologue's sub_rsp +
                // extra_pushes plus the RSM record (Self / static-link
                // special cases). We just need to tell gdb that
                // DW_AT_frame_base = RBP and DW_OP_fbreg(N) means [rbp+N].
                info.uleb128(kAbbrevSubprogramWithKids);
                info.cstr(sym.name);
                info.u64(sym.address);
                info.u64(end);
                info.u8(1);                       // external
                // frame_base exprloc: DW_OP_breg6 sleb128(0) -- frame
                // base is RBP itself; each var is `[rbp + stack_offset]`.
                {
                    auto leb = sleb128Bytes(0);
                    info.uleb128(1 + leb.size());
                    info.u8(DW_OP_breg6);
                    info.raw(leb.data(), leb.size());
                }

                auto emitParamOrLocal = [&](const model::LocalVar& v,
                                            std::uint64_t abbrev_code) {
                    std::uint32_t type_off = 0;
                    if (v.type != model::kNoType) {
                        auto it = type_offsets.find(v.type);
                        if (it != type_offsets.end()) type_off = it->second;
                    }
                    info.uleb128(abbrev_code);
                    info.cstr(v.name);
                    info.u32(type_off);
                    // Location: DW_OP_fbreg sleb128(stack_offset).
                    // stack_offset is already rbp-relative real byte
                    // offset (post-compose); no scaling needed.
                    auto leb = sleb128Bytes(v.stack_offset);
                    info.uleb128(1 + leb.size());
                    info.u8(DW_OP_fbreg);
                    info.raw(leb.data(), leb.size());
                };

                for (const auto& p : sym.params)
                    emitParamOrLocal(p, kAbbrevFormalParameter);
                for (const auto& l : sym.locals)
                    emitParamOrLocal(l, kAbbrevLocalVariable);

                info.u8(0);                       // children-end marker
            }
            break;
        }
        case model::SymbolKind::Variable: {
            // Choose typed-variable abbrev (4) when we have a resolved
            // type DIE; otherwise fall back to the untyped abbrev (3).
            std::uint32_t type_off = 0;
            if (sym.type != model::kNoType) {
                auto it = type_offsets.find(sym.type);
                if (it != type_offsets.end()) type_off = it->second;
            }
            if (type_off != 0) {
                info.uleb128(kAbbrevTypedVariable);
                info.cstr(sym.name);
                info.uleb128(1 + 8);
                info.u8(DW_OP_addr);
                info.u64(sym.address);
                info.u32(type_off);
                info.u8(1);  // external
            } else {
                info.uleb128(kAbbrevVariable);
                info.cstr(sym.name);
                info.uleb128(1 + 8);
                info.u8(DW_OP_addr);
                info.u64(sym.address);
                info.u8(1);  // external
            }
            break;
        }
        case model::SymbolKind::Unknown:
            // Skip; we don't know how to describe it.
            break;
        }
    }

    // End-of-children marker for the CU.
    info.u8(0);

    // Patch unit_length.
    std::size_t total_after_length = info.size() - length_field_end;
    info.patchU32(unit_length_pos, static_cast<std::uint32_t>(total_after_length));
}

} // namespace

// =========================================================================
//                                  emit
// =========================================================================

bool emit(const model::Module& mod,
          const EmitOptions& opts,
          DwarfSections& out,
          std::string& error_out) {
    if (mod.units.empty()) {
        error_out = "model has no compile units";
        return false;
    }

    BytesBuf info, abbrev, line, str, line_str;

    writeAbbrevTable(abbrev);
    const std::uint32_t abbrev_offset = 0;   // single table; CU refers to byte 0

    for (const auto& cu : mod.units) {
        if (cu.lines.empty()) continue;       // skip CUs with no line info
        const std::uint32_t stmt_list_off = writeLineProgram(line, cu);
        writeCompileUnit(info, mod, cu, opts.producer, stmt_list_off, abbrev_offset);
    }

    out.debug_info     = std::move(info).take();
    out.debug_abbrev   = std::move(abbrev).take();
    out.debug_line     = std::move(line).take();
    out.debug_str      = std::move(str).take();     // empty
    out.debug_line_str = std::move(line_str).take(); // empty

    return true;
}

} // namespace rsm2pdb::dwarf
