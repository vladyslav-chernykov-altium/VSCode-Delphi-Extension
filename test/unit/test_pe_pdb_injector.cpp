// Byte-level test for the PE -> PDB RSDS injector. We use the hello.exe
// fixture (committed Delphi build) as input, inject an RSDS pointer
// for a known GUID/age/basename, then walk the resulting PE to verify:
//   - DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] points at a new
//     .debug section.
//   - That section starts with a valid IMAGE_DEBUG_DIRECTORY whose
//     Type = IMAGE_DEBUG_TYPE_CODEVIEW.
//   - The RSDS payload that follows carries our GUID + age + path.

#include "doctest/doctest.h"

#include "pe/pe_pdb_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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

const std::uint8_t* peSectionDataAtRva(
    const std::vector<std::uint8_t>& pe, std::uint32_t rva,
    std::uint32_t size) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data());
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        pe.data() + dos->e_lfanew);
    const auto* secs = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(nt)
            + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
            + nt->FileHeader.SizeOfOptionalHeader);
    for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = secs[i];
        if (rva >= s.VirtualAddress &&
            rva + size <= s.VirtualAddress +
                std::max(s.SizeOfRawData, s.Misc.VirtualSize)) {
            return pe.data() + s.PointerToRawData +
                   (rva - s.VirtualAddress);
        }
    }
    return nullptr;
}

} // namespace


TEST_CASE("PE PDB injector: RSDS roundtrip on hello.exe fixture") {
    const auto fixture =
        fs::path(RSM2PDB_FIXTURES_DIR) / "hello.exe";
    auto in = readFile(fixture);
    REQUIRE(in.size() > 0);

    const rsm2pdb::pdb::Guid guid = {
        0xDE,0xAD,0xBE,0xEF, 0xCA,0xFE, 0xBA,0xBE,
        0x12,0x34,0x56,0x78, 0x9A,0xBC,0xDE,0xF0,
    };
    const std::uint32_t age = 42;
    const std::string pdb_name = "hello-test.pdb";

    std::vector<std::uint8_t> out;
    std::string err;
    REQUIRE(rsm2pdb::pe::injectPdbReference(in, guid, age, pdb_name, out, err));
    CHECK(out.size() >= in.size());

    // DataDirectory[DEBUG] must now be populated.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(out.data());
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        out.data() + dos->e_lfanew);
    const auto& dd  =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    REQUIRE(dd.VirtualAddress != 0);
    CHECK(dd.Size == sizeof(IMAGE_DEBUG_DIRECTORY));

    // Walk to the IMAGE_DEBUG_DIRECTORY -> CODEVIEW payload.
    const auto* dbg = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(
        peSectionDataAtRva(out, dd.VirtualAddress, dd.Size));
    REQUIRE(dbg != nullptr);
    CHECK(dbg->Type == IMAGE_DEBUG_TYPE_CODEVIEW);
    CHECK(dbg->SizeOfData == 4u + 16u + 4u + pdb_name.size() + 1u);

    const auto* rsds = peSectionDataAtRva(
        out, dbg->AddressOfRawData, dbg->SizeOfData);
    REQUIRE(rsds != nullptr);
    CHECK(std::memcmp(rsds, "RSDS", 4) == 0);
    CHECK(std::memcmp(rsds + 4, guid.data(), 16) == 0);
    std::uint32_t out_age = 0;
    std::memcpy(&out_age, rsds + 20, 4);
    CHECK(out_age == age);
    CHECK(std::string(reinterpret_cast<const char*>(rsds + 24)) == pdb_name);
}
