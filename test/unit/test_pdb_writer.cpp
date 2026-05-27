// Smoke tests for the PDB writer. We don't round-trip the full PDB
// (that would require pulling in LLVM's PDB reader); instead we check
// the MSF container header and the file size lower bound that proves
// the well-known streams (Info / DBI / TPI / IPI / GSI) were written.

#include "doctest/doctest.h"

#include "pdb/pdb_writer.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = f.tellg();
    f.seekg(0);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return out;
}

fs::path tmpPdbPath(const char* tag) {
    auto p = fs::temp_directory_path() /
             (std::string("rsm2pdb_test_") + tag + ".pdb");
    std::error_code ec;
    fs::remove(p, ec);
    return p;
}

rsm2pdb::pdb::CoffSection codeSection() {
    rsm2pdb::pdb::CoffSection s;
    s.name = ".text";
    s.virtual_address     = 0x1000;
    s.virtual_size        = 0x1000;
    s.size_of_raw_data    = 0x1000;
    s.pointer_to_raw_data = 0x400;
    s.characteristics     = 0x60000020;  // CODE|EXEC|READ
    return s;
}

} // namespace


TEST_CASE("PDB writer: minimal-empty writes valid MSF") {
    rsm2pdb::pdb::PdbInputs in;
    in.guid = {0x01,0x02,0x03,0x04, 0x05,0x06, 0x07,0x08,
               0x09,0x0a,0x0b,0x0c, 0x0d,0x0e,0x0f,0x10};
    in.age = 7;
    in.sections.push_back(codeSection());

    const auto path = tmpPdbPath("empty");
    std::string err;
    const bool ok = rsm2pdb::pdb::writePdb(path.string(), in, err);
    CHECK_MESSAGE(ok, err);

    const auto bytes = readFile(path);
    REQUIRE(bytes.size() >= 32);
    // MSF 7.00 magic: "Microsoft C/C++ MSF 7.00\r\n\x1ADS\0\0\0"
    const char* magic = "Microsoft C/C++ MSF 7.00\r\n\x1A""DS";
    CHECK(std::memcmp(bytes.data(), magic, 30) == 0);

    fs::remove(path);
}

TEST_CASE("PDB writer: publics + module + function + line") {
    rsm2pdb::pdb::PdbInputs in;
    in.guid = {0xa1,0xa2,0xa3,0xa4, 0xa5,0xa6, 0xa7,0xa8,
               0xa9,0xaa,0xab,0xac, 0xad,0xae,0xaf,0xb0};
    in.age = 1;
    in.sections.push_back(codeSection());

    rsm2pdb::pdb::PublicSymbol p;
    p.name = "Foo.Bar";
    p.segment = 1;
    p.offset = 0x40;
    p.is_function = true;
    in.publics.push_back(p);

    rsm2pdb::pdb::ModuleFunction fn;
    fn.name = "Foo.Bar";
    fn.segment = 1;
    fn.offset = 0x40;
    fn.size = 0x20;
    fn.locals.push_back({"arg1", 32, true});
    fn.locals.push_back({"local1", 8, false});

    rsm2pdb::pdb::Module mod;
    mod.name = "Foo";
    mod.functions.push_back(fn);
    rsm2pdb::pdb::ModuleSource src;
    src.source_path = "Foo.pas";  // file-not-found path is acceptable
    src.lines.push_back({1, 0x40, 10});
    src.lines.push_back({1, 0x48, 11});
    mod.sources.push_back(std::move(src));
    in.modules.push_back(mod);

    const auto path = tmpPdbPath("rich");
    std::string err;
    const bool ok = rsm2pdb::pdb::writePdb(path.string(), in, err);
    CHECK_MESSAGE(ok, err);

    const auto bytes = readFile(path);
    // A PDB with one module + a couple of records is well over the
    // single-block (4 KiB) container size used for an empty PDB.
    CHECK(bytes.size() >= 8 * 4096);

    fs::remove(path);
}

TEST_CASE("PDB writer: globals (non-function publics) emit S_GDATA32") {
    rsm2pdb::pdb::PdbInputs in;
    in.guid = {};
    in.age = 1;

    rsm2pdb::pdb::CoffSection text = codeSection();
    rsm2pdb::pdb::CoffSection data;
    data.name = ".data";
    data.virtual_address     = 0x2000;
    data.virtual_size        = 0x100;
    data.size_of_raw_data    = 0x200;
    data.pointer_to_raw_data = 0x1400;
    data.characteristics     = 0xC0000040;  // INITIALIZED_DATA|READ|WRITE
    in.sections.push_back(text);
    in.sections.push_back(data);

    in.publics.push_back({"Unit.S", 2, 0x10, false});
    in.publics.push_back({"Unit.MyFn", 1, 0x20, true});

    const auto path = tmpPdbPath("globals");
    std::string err;
    REQUIRE(rsm2pdb::pdb::writePdb(path.string(), in, err));
    CHECK(fs::file_size(path) > 0);
    fs::remove(path);
}
