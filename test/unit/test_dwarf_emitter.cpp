// DWARF emitter tests.
//
// We verify the emitter at the byte level rather than via
// llvm::DWARFContext readback, which proved finicky to set up for
// raw in-memory sections. The byte-level checks catch structural
// regressions; gdb's view of the produced bytes (after PE injection)
// is the final acceptance test.

#include <doctest/doctest.h>

#include "dwarf/dwarf_emitter.h"
#include "model/model.h"

#include "llvm/BinaryFormat/Dwarf.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

std::uint32_t readU32LE(const std::vector<std::uint8_t>& d, std::size_t off) {
    return  std::uint32_t(d[off])         |
           (std::uint32_t(d[off + 1]) <<  8) |
           (std::uint32_t(d[off + 2]) << 16) |
           (std::uint32_t(d[off + 3]) << 24);
}
std::uint16_t readU16LE(const std::vector<std::uint8_t>& d, std::size_t off) {
    return std::uint16_t(d[off]) | (std::uint16_t(d[off + 1]) << 8);
}
std::uint64_t readU64LE(const std::vector<std::uint8_t>& d, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(d[off + i]) << (i * 8);
    return v;
}

rsm2pdb::model::Module simpleSingleCuModel() {
    rsm2pdb::model::Module m;
    m.source_files.push_back("hello.dpr");
    rsm2pdb::model::CompileUnit cu;
    cu.source_path = "C:/some/path/hello.dpr";
    cu.lines.push_back({0x140000000ull + 0x25900, 0, 24});
    cu.lines.push_back({0x140000000ull + 0x2590E, 0, 25});
    cu.lines.push_back({0x140000000ull + 0x25917, 0, 26});
    cu.lines.push_back({0x140000000ull + 0x2591D, 0, 27});
    m.units.push_back(cu);
    return m;
}

bool containsBytes(const std::vector<std::uint8_t>& haystack,
                   const std::string& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        if (std::memcmp(haystack.data() + i, needle.data(), needle.size()) == 0)
            return true;
    }
    return false;
}

} // namespace


TEST_CASE("Dwarf emitter: empty module fails cleanly") {
    rsm2pdb::model::Module m;
    rsm2pdb::dwarf::EmitOptions opts;
    rsm2pdb::dwarf::DwarfSections out;
    std::string err;
    CHECK_FALSE(rsm2pdb::dwarf::emit(m, opts, out, err));
    CHECK(!err.empty());
}


TEST_CASE("Dwarf emitter: section byte layout for single CU") {
    auto m = simpleSingleCuModel();
    rsm2pdb::dwarf::EmitOptions opts;
    rsm2pdb::dwarf::DwarfSections out;
    std::string err;
    REQUIRE(rsm2pdb::dwarf::emit(m, opts, out, err));

    REQUIRE(out.debug_info.size() >= 12);  // at least the CU header
    REQUIRE(out.debug_abbrev.size() >= 8); // abbrev table not empty
    REQUIRE(out.debug_line.size()  >= 12); // line program header

    // .debug_info: CU header layout (DWARF v5, 32-bit DWARF)
    // [0..3]  unit_length (excludes this field)
    // [4..5]  version = 5
    // [6]     unit_type = DW_UT_compile (1)
    // [7]     address_size = 8
    // [8..11] debug_abbrev_offset = 0
    SUBCASE("CU header fields") {
        std::uint32_t unit_length = readU32LE(out.debug_info, 0);
        CHECK(unit_length + 4 == out.debug_info.size());

        std::uint16_t version = readU16LE(out.debug_info, 4);
        CHECK(version == 5);
        CHECK(out.debug_info[6] == llvm::dwarf::DW_UT_compile);
        CHECK(out.debug_info[7] == 8);

        std::uint32_t abbrev_off = readU32LE(out.debug_info, 8);
        CHECK(abbrev_off == 0);
    }

    SUBCASE("CU DIE: producer / language strings present") {
        CHECK(containsBytes(out.debug_info, "rsm2pdb"));
        CHECK(containsBytes(out.debug_info, "hello.dpr"));
        CHECK(containsBytes(out.debug_info, "C:/some/path"));
    }

    SUBCASE(".debug_abbrev encodes compile_unit + subprogram") {
        // Abbrev 1: TAG_compile_unit, CHILDREN_yes (for subprogram kids).
        REQUIRE(out.debug_abbrev[0] == 0x01);
        CHECK(out.debug_abbrev[1] == llvm::dwarf::DW_TAG_compile_unit);
        CHECK(out.debug_abbrev[2] == llvm::dwarf::DW_CHILDREN_yes);
        // Table ends with a 0 byte (end-of-table marker).
        CHECK(out.debug_abbrev.back() == 0);
        // The second abbrev (DW_TAG_subprogram) appears later in the table.
        CHECK(containsBytes(out.debug_abbrev,
            std::string({char(0x02),
                         char(llvm::dwarf::DW_TAG_subprogram),
                         char(llvm::dwarf::DW_CHILDREN_no)})));
    }
}


TEST_CASE("Dwarf emitter: line program contains correct addresses") {
    auto m = simpleSingleCuModel();
    rsm2pdb::dwarf::DwarfSections out;
    std::string err;
    REQUIRE(rsm2pdb::dwarf::emit(m, {}, out, err));

    // .debug_line header (DWARF v5):
    //   [0..3]  unit_length
    //   [4..5]  version = 5
    //   [6]     address_size = 8
    //   [7]     segment_selector_size = 0
    //   [8..11] header_length
    std::uint16_t version = readU16LE(out.debug_line, 4);
    CHECK(version == 5);
    CHECK(out.debug_line[6] == 8);
    CHECK(out.debug_line[7] == 0);

    // The first address we set via DW_LNE_set_address should appear
    // verbatim as 8 bytes in the line program (after header).
    std::uint64_t first_addr = 0x140000000ull + 0x25900;
    std::uint8_t addr_bytes[8];
    for (int i = 0; i < 8; ++i)
        addr_bytes[i] = static_cast<std::uint8_t>(first_addr >> (i * 8));
    CHECK(containsBytes(out.debug_line,
                        std::string(reinterpret_cast<const char*>(addr_bytes), 8)));

    // Regression: must emit DW_LNS_set_file 0 at the start of the
    // line program. gdb/LLVM initialize the state machine's File
    // register to 1, but our file table has only entry 0. Without
    // an explicit set_file, every line row references file 1 and
    // is silently dropped.
    // After the line program header (which ends after the file
    // table), the next two bytes should be DW_LNS_set_file (0x04)
    // followed by ULEB128 0 (just byte 0x00).
    // The header length is at bytes [8..11]; total header is
    // 12 + header_length.
    std::uint32_t header_length = readU32LE(out.debug_line, 8);
    std::size_t program_start = 12 + header_length;
    REQUIRE(out.debug_line.size() > program_start + 1);
    CHECK(out.debug_line[program_start    ] == llvm::dwarf::DW_LNS_set_file);
    CHECK(out.debug_line[program_start + 1] == 0);
}


TEST_CASE("Dwarf emitter: multi-CU module emits two CU headers") {
    rsm2pdb::model::Module m;
    m.source_files.push_back("a.pas");
    m.source_files.push_back("b.pas");
    {
        rsm2pdb::model::CompileUnit cu;
        cu.source_path = "a.pas";
        cu.lines.push_back({0x1000, 0, 10});
        cu.lines.push_back({0x1010, 0, 11});
        m.units.push_back(cu);
    }
    {
        rsm2pdb::model::CompileUnit cu;
        cu.source_path = "b.pas";
        cu.lines.push_back({0x2000, 1, 20});
        cu.lines.push_back({0x2010, 1, 21});
        m.units.push_back(cu);
    }

    rsm2pdb::dwarf::DwarfSections out;
    std::string err;
    REQUIRE(rsm2pdb::dwarf::emit(m, {}, out, err));

    // Walk .debug_info CU by CU and count.
    int cu_count = 0;
    std::size_t off = 0;
    while (off + 4 <= out.debug_info.size()) {
        std::uint32_t unit_length = readU32LE(out.debug_info, off);
        if (unit_length == 0) break;
        off += 4 + unit_length;
        ++cu_count;
    }
    CHECK(cu_count == 2);

    // Both file names should appear verbatim.
    CHECK(containsBytes(out.debug_info, "a.pas"));
    CHECK(containsBytes(out.debug_info, "b.pas"));
}
