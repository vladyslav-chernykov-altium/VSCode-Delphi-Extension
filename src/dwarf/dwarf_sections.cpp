// dwarf_sections.cpp -- .debug_abbrev + .debug_line writers.

#include "dwarf/dwarf_emitter.h"
#include "dwarf/dwarf_internal.h"

#include "llvm/BinaryFormat/Dwarf.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace rsm2pdb::dwarf::detail {

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

} // namespace rsm2pdb::dwarf::detail
