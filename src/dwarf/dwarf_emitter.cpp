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
#include <sstream>
#include <string>
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
// All CUs share abbreviation code 1; subprograms share code 2.
// ---------------------------------------------------------------------
constexpr std::uint64_t kAbbrevCu         = 1;
constexpr std::uint64_t kAbbrevSubprogram = 2;

void writeAbbrevTable(BytesBuf& abbrev) {
    using namespace llvm::dwarf;

    // Abbrev 1: DW_TAG_compile_unit, HAS children (subprograms are
    // emitted as children of the CU DIE).
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

    // Abbrev 2: DW_TAG_subprogram, no children (no params/locals in D2).
    abbrev.uleb128(kAbbrevSubprogram);
    abbrev.uleb128(DW_TAG_subprogram);
    abbrev.u8(DW_CHILDREN_no);
    abbrev.uleb128(DW_AT_name);      abbrev.uleb128(DW_FORM_string);
    abbrev.uleb128(DW_AT_low_pc);    abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_high_pc);   abbrev.uleb128(DW_FORM_addr);
    abbrev.uleb128(DW_AT_external);  abbrev.uleb128(DW_FORM_flag);
    abbrev.uleb128(0);               abbrev.uleb128(0);

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
// ---------------------------------------------------------------------
struct AddressRange { std::uint64_t lo, hi; };
AddressRange cuRange(const model::CompileUnit& cu) {
    AddressRange r{0, 0};
    if (cu.lines.empty()) return r;
    r.lo = cu.lines.front().address;
    r.hi = cu.lines.front().address;
    for (const auto& le : cu.lines) {
        if (le.address < r.lo) r.lo = le.address;
        if (le.address > r.hi) r.hi = le.address;
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
// Compile-unit DIE for .debug_info.
// ---------------------------------------------------------------------
void writeCompileUnit(BytesBuf& info,
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

    // ---- Child DIEs: one DW_TAG_subprogram per public function ----
    // We filter to symbols whose address falls within the CU's
    // address range. Aux symbols ($pdata$, $unwind$) are already
    // filtered by the .map adapter.
    for (const auto& sym : cu.symbols) {
        if (sym.address < rng.lo || sym.address > rng.hi) continue;

        std::uint64_t end = symbolEnd(cu, sym, rng.hi + 1);
        info.uleb128(kAbbrevSubprogram);
        info.cstr(sym.name);              // DW_AT_name   (string)
        info.u64(sym.address);            // DW_AT_low_pc (addr)
        info.u64(end);                    // DW_AT_high_pc (addr, exclusive)
        info.u8(1);                       // DW_AT_external = true (flag)
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
        writeCompileUnit(info, cu, opts.producer, stmt_list_off, abbrev_offset);
    }

    out.debug_info     = std::move(info).take();
    out.debug_abbrev   = std::move(abbrev).take();
    out.debug_line     = std::move(line).take();
    out.debug_str      = std::move(str).take();     // empty
    out.debug_line_str = std::move(line_str).take(); // empty

    return true;
}

} // namespace rsm2pdb::dwarf
