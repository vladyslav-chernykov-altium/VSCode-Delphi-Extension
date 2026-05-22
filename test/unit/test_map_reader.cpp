#include <doctest/doctest.h>

#include "map/map_reader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// Tiny synthetic map snippets for parse testing. We use a temp file
// because Reader::open() takes a path.

std::string writeTemp(const std::string& contents) {
    auto p = std::filesystem::temp_directory_path() /
             ("rsm2pdb_test_map_" + std::to_string(::rand()) + ".map");
    std::ofstream f(p, std::ios::binary);
    f << contents;
    return p.string();
}

} // namespace

TEST_CASE("MapReader: parses segment overview") {
    const std::string text =
        "\n"
        " Start         Length     Name                   Class\n"
        " 0001:00401000 00029194H .text                   CODE\n"
        " 0002:0042B000 00004AF8H .data                   DATA\n"
        "\n";
    auto path = writeTemp(text);
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(path));
    const auto& f = r.file();
    REQUIRE(f.segments.size() == 2);
    CHECK(f.segments[0].id == 1);
    CHECK(f.segments[0].start_va == 0x401000);
    CHECK(f.segments[0].length == 0x29194);
    CHECK(f.segments[0].name == ".text");
    CHECK(f.segments[0].klass == "CODE");
    CHECK(f.segments[1].id == 2);
    CHECK(f.segments[1].name == ".data");
}

TEST_CASE("MapReader: parses detailed map of segments") {
    const std::string text =
        "\n"
        "Detailed map of segments\n"
        "\n"
        " 0001:00000000 000100BC C=CODE     S=.text    G=(none)   M=System   ALIGN=4\n"
        " 0001:00025900 00003894 C=CODE     S=.text    G=(none)   M=hello    ALIGN=4\n"
        "\n";
    auto path = writeTemp(text);
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(path));
    const auto& f = r.file();
    REQUIRE(f.module_segments.size() == 2);
    CHECK(f.module_segments[1].segment_id == 1);
    CHECK(f.module_segments[1].segment_offset == 0x25900);
    CHECK(f.module_segments[1].length == 0x3894);
    CHECK(f.module_segments[1].module_name == "hello");
    CHECK(f.module_segments[1].klass == "CODE");
    CHECK(f.module_segments[1].section == ".text");
    CHECK(f.module_segments[1].alignment == 4);
}

TEST_CASE("MapReader: parses publics by name") {
    const std::string text =
        "\n"
        "  Address             Publics by Name\n"
        "\n"
        " 0001:00025900       hello.Add\n"
        " 0001:00025930       hello.Finalization\n"
        "\n";
    auto path = writeTemp(text);
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(path));
    const auto& f = r.file();
    REQUIRE(f.publics.size() == 2);
    CHECK(f.publics[0].segment_id == 1);
    CHECK(f.publics[0].segment_offset == 0x25900);
    CHECK(f.publics[0].name == "hello.Add");
}

TEST_CASE("MapReader: parses line numbers section") {
    const std::string text =
        "\n"
        "Line numbers for hello(hello.dpr) segment .text\n"
        "\n"
        "    24 0001:00025900    25 0001:0002590E    26 0001:00025917    27 0001:0002591D\n"
        "    34 0001:00025940\n"
        "\n";
    auto path = writeTemp(text);
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(path));
    const auto& f = r.file();
    REQUIRE(f.line_tables.size() == 1);
    const auto& lt = f.line_tables[0];
    CHECK(lt.module_name == "hello");
    CHECK(lt.source_path == "hello.dpr");
    CHECK(lt.segment_name == ".text");
    REQUIRE(lt.lines.size() == 5);
    CHECK(lt.lines[0].line == 24);
    CHECK(lt.lines[0].segment_offset == 0x25900);
    CHECK(lt.lines[3].line == 27);
    CHECK(lt.lines[3].segment_offset == 0x2591D);
    CHECK(lt.lines[4].line == 34);
}

TEST_CASE("MapReader: parses program entry point") {
    const std::string text =
        "\n"
        "Program entry point at 0001:00025940\n";
    auto path = writeTemp(text);
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(path));
    const auto& f = r.file();
    REQUIRE(f.entry_point.has_value);
    CHECK(f.entry_point.segment_id == 1);
    CHECK(f.entry_point.segment_offset == 0x25940);
}

TEST_CASE("MapReader: populates model::Module from MapFile") {
    rsm2pdb::map::MapFile mf;
    rsm2pdb::map::Segment seg{};
    seg.id = 1;
    seg.start_va = 0x401000;
    seg.length = 0x29194;
    seg.name = ".text";
    seg.klass = "CODE";
    mf.segments.push_back(seg);

    rsm2pdb::map::Public pub{};
    pub.segment_id = 1;
    pub.segment_offset = 0x25900;
    pub.name = "hello.Add";
    mf.publics.push_back(pub);

    rsm2pdb::map::LineTable lt{};
    lt.module_name = "hello";
    lt.source_path = "hello.dpr";
    lt.segment_name = ".text";
    lt.lines.push_back({24, 1, 0x25900});
    lt.lines.push_back({25, 1, 0x2590E});
    mf.line_tables.push_back(lt);

    rsm2pdb::model::Module mod;
    rsm2pdb::map::populate(mf, mod);

    REQUIRE(mod.source_files.size() == 1);
    CHECK(mod.source_files[0] == "hello.dpr");
    REQUIRE(mod.units.size() == 1);

    const auto& cu = mod.units[0];
    CHECK(cu.source_path == "hello.dpr");
    REQUIRE(cu.lines.size() == 2);
    // Addresses absolutized through the segment table.
    CHECK(cu.lines[0].address == 0x401000 + 0x25900);
    CHECK(cu.lines[1].address == 0x401000 + 0x2590E);
    REQUIRE(cu.symbols.size() == 1);
    // Full qualified name preserved (we keep "hello.Add" not "Add"
    // so stack traces show "hello.Add" and break-by-name works).
    CHECK(cu.symbols[0].name == "hello.Add");
    CHECK(cu.symbols[0].address == 0x401000 + 0x25900);
}
