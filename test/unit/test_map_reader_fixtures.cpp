// Integration tests against committed real-world map files in
// test/fixtures/. Touches the real parser + adapter end-to-end on
// the kinds of Delphi projects we expect to support.

#include <doctest/doctest.h>

#include "map/map_reader.h"
#include "model/model.h"

#include <algorithm>
#include <string>

#ifndef RSM2PDB_FIXTURES_DIR
#  error "RSM2PDB_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

const rsm2pdb::map::Public* findPublic(const rsm2pdb::map::MapFile& f,
                                       const std::string& name) {
    for (const auto& p : f.publics) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const rsm2pdb::map::LineTable* findLineTable(const rsm2pdb::map::MapFile& f,
                                             const std::string& module) {
    for (const auto& lt : f.line_tables) {
        if (lt.module_name == module) return &lt;
    }
    return nullptr;
}

const rsm2pdb::model::Symbol* findSymbol(const rsm2pdb::model::CompileUnit& cu,
                                         const std::string& name) {
    for (const auto& s : cu.symbols) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

} // namespace


TEST_CASE("Fixture hello.map: single-unit baseline") {
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(std::string(RSM2PDB_FIXTURES_DIR) + "/hello.map"));
    const auto& f = r.file();

    // Five segments .text .data .bss .tls .pdata in this order.
    REQUIRE(f.segments.size() == 5);
    CHECK(f.segments[0].name == ".text");
    CHECK(f.segments[4].name == ".pdata");

    // One line table for hello.
    REQUIRE(f.line_tables.size() == 1);
    const auto* lt = findLineTable(f, "hello");
    REQUIRE(lt != nullptr);
    CHECK(lt->source_path == "hello.dpr");
    CHECK(lt->segment_name == ".text");
    CHECK(lt->lines.size() == 11);
    // First line: function Add() begins at 24.
    CHECK(lt->lines.front().line == 24);
    CHECK(lt->lines.front().segment_offset == 0x25900);
    // Last line: program end. at 40.
    CHECK(lt->lines.back().line == 40);
    CHECK(lt->lines.back().segment_offset == 0x259DE);

    // hello.Add public.
    const auto* p = findPublic(f, "hello.Add");
    REQUIRE(p != nullptr);
    CHECK(p->segment_id == 1);
    CHECK(p->segment_offset == 0x25900);

    // Entry point.
    REQUIRE(f.entry_point.has_value);
    CHECK(f.entry_point.segment_offset == 0x25940);
}


TEST_CASE("Fixture two_units.map: multi-unit with dotted unit name") {
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(std::string(RSM2PDB_FIXTURES_DIR) + "/two_units.map"));
    const auto& f = r.file();

    // Three line tables (one per user unit).
    REQUIRE(f.line_tables.size() == 3);
    CHECK(findLineTable(f, "Geometry")   != nullptr);
    CHECK(findLineTable(f, "App.Colors") != nullptr);
    CHECK(findLineTable(f, "two_units")  != nullptr);

    // Dotted unit name handling: App.Colors has a source App.Colors.pas
    // and its publics include "App.Colors.ColorName".
    {
        const auto* lt = findLineTable(f, "App.Colors");
        REQUIRE(lt != nullptr);
        CHECK(lt->source_path == "App.Colors.pas");
        CHECK(lt->lines.size() == 8);
    }
    CHECK(findPublic(f, "App.Colors.ColorName") != nullptr);

    // Cross-unit function: Add lives in Geometry, called from main.
    const auto* add = findPublic(f, "Geometry.Add");
    REQUIRE(add != nullptr);
    CHECK(add->segment_id == 1);
    CHECK(add->segment_offset == 0x25900);
}


TEST_CASE("Fixture two_units.map: adapter keeps qualified names, filters aux") {
    rsm2pdb::map::Reader r;
    REQUIRE(r.open(std::string(RSM2PDB_FIXTURES_DIR) + "/two_units.map"));
    const auto& mf = r.file();

    rsm2pdb::model::Module mod;
    rsm2pdb::map::populate(mf, mod);

    // 3 CompileUnits, one per source file the .map advertised.
    REQUIRE(mod.units.size() == 3);

    const rsm2pdb::model::CompileUnit* colors_cu = nullptr;
    for (const auto& cu : mod.units) {
        if (cu.source_path == "App.Colors.pas") { colors_cu = &cu; break; }
    }
    REQUIRE(colors_cu != nullptr);
    // Full qualified name preserved.
    const auto* color_name = findSymbol(*colors_cu, "App.Colors.ColorName");
    REQUIRE(color_name != nullptr);
    CHECK(color_name->address == 0x401000 + 0x25990);

    // Geometry CU: Geometry.Add@0x25900 and Geometry.DistanceSq present.
    const rsm2pdb::model::CompileUnit* geo_cu = nullptr;
    for (const auto& cu : mod.units) {
        if (cu.source_path == "Geometry.pas") { geo_cu = &cu; break; }
    }
    REQUIRE(geo_cu != nullptr);
    const auto* add = findSymbol(*geo_cu, "Geometry.Add");
    REQUIRE(add != nullptr);
    CHECK(add->address == 0x401000 + 0x25900);
    CHECK(findSymbol(*geo_cu, "Geometry.DistanceSq") != nullptr);

    // Aux EH symbols must be filtered out.
    CHECK(findSymbol(*geo_cu, "Geometry.$pdata$_ZN8Geometry3AddEii") == nullptr);
    CHECK(findSymbol(*geo_cu, "Geometry.$unwind$_ZN8Geometry3AddEii") == nullptr);
    // The "Geometry..1" anonymous-internal symbol should be filtered.
    for (const auto& s : geo_cu->symbols) {
        CHECK(s.name.find("..") == std::string::npos);
    }
}
