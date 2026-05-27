// PE injector for the CodeView (RSDS) Debug Directory entry.
//
// Layout we add to the input PE:
//
//   New section ".debug" (read-only, INITIALIZED_DATA):
//     +0    IMAGE_DEBUG_DIRECTORY      (28 bytes)
//     +28   RSDS payload:
//             "RSDS"                   (4)
//             GUID                     (16, raw bytes)
//             Age                      (u32 LE)
//             pdb_basename             (NUL-terminated)
//
//   IMAGE_DEBUG_DIRECTORY fields:
//     Type             = IMAGE_DEBUG_TYPE_CODEVIEW (2)
//     SizeOfData       = 4 + 16 + 4 + len(basename) + 1
//     AddressOfRawData = section_va + 28
//     PointerToRawData = section_fo + 28
//
//   OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]:
//     VirtualAddress = section_va
//     Size           = sizeof(IMAGE_DEBUG_DIRECTORY) = 28
//
// Header-room expansion, COFF symbol-table preservation, and string-
// table extension follow the same approach as pe_injector.cpp; the
// difference is we add exactly one section and patch DataDirectory[6].

#include "pe/pe_pdb_injector.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace rsm2pdb::pe {

namespace {

std::uint32_t roundUp(std::uint32_t v, std::uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

bool readFile(const std::string& path,
              std::vector<std::uint8_t>& out,
              std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open input PE: " + path; return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string& s = ss.str();
    out.assign(s.begin(), s.end());
    return true;
}

bool writeFile(const std::string& path,
               const std::vector<std::uint8_t>& bytes,
               std::string& err) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot open output PE: " + path; return false; }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) { err = "write failed: " + path; return false; }
    return true;
}

} // namespace


bool injectPdbReference(const std::vector<std::uint8_t>& pe_in,
                        const pdb::Guid& guid,
                        std::uint32_t age,
                        const std::string& pdb_basename,
                        std::vector<std::uint8_t>& pe_out,
                        std::string& error_out) {
    if (pe_in.size() < sizeof(IMAGE_DOS_HEADER)) {
        error_out = "input too small to be a PE";
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pe_in.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        error_out = "input is not a PE (bad DOS magic)";
        return false;
    }
    if (dos->e_lfanew <= 0 ||
        static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > pe_in.size()) {
        error_out = "bad e_lfanew";
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        pe_in.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        error_out = "input is not a PE (bad NT signature)";
        return false;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        error_out = "only x64 PE images are supported";
        return false;
    }

    const std::uint16_t tgt_n = nt->FileHeader.NumberOfSections;
    const std::uint32_t file_align  = nt->OptionalHeader.FileAlignment;
    const std::uint32_t sect_align  = nt->OptionalHeader.SectionAlignment;
    const std::uint32_t size_of_headers = nt->OptionalHeader.SizeOfHeaders;

    const auto* tgt_secs = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(nt)
            + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
            + nt->FileHeader.SizeOfOptionalHeader);

    // Build the RSDS payload + IMAGE_DEBUG_DIRECTORY block. We compute
    // the section's final va/fo *after* layout below, so the directory
    // entry's AddressOfRawData/PointerToRawData are filled in last.
    const std::uint32_t rsds_size = 4u + 16u + 4u +
        static_cast<std::uint32_t>(pdb_basename.size() + 1u);
    const std::uint32_t dbg_dir_size = sizeof(IMAGE_DEBUG_DIRECTORY);
    const std::uint32_t section_payload_size = dbg_dir_size + rsds_size;

    // ---- Header room: one new section header needed ----
    const std::uint32_t sect_table_end_off =
        static_cast<std::uint32_t>(
            reinterpret_cast<const std::uint8_t*>(tgt_secs + tgt_n) -
            pe_in.data());
    const std::uint32_t header_free  = size_of_headers - sect_table_end_off;
    const std::uint32_t header_need  = sizeof(IMAGE_SECTION_HEADER);
    std::uint32_t header_expansion = 0;
    if (header_need > header_free) {
        header_expansion = roundUp(header_need - header_free, file_align);
    }

    // ---- Preserve existing COFF symbol + string table ----
    const std::uint32_t old_symtab_offset = nt->FileHeader.PointerToSymbolTable;
    const std::uint32_t old_n_symbols    = nt->FileHeader.NumberOfSymbols;
    const std::uint32_t old_symtab_size  = old_n_symbols * 18u;
    std::uint32_t old_strtab_size = 4;
    if (old_symtab_offset != 0) {
        if (old_symtab_offset + old_symtab_size + 4 > pe_in.size()) {
            error_out = "symbol table out of bounds";
            return false;
        }
        std::memcpy(&old_strtab_size,
                    pe_in.data() + old_symtab_offset + old_symtab_size, 4);
    }

    // ---- File layout: where does our new section land? ----
    std::uint64_t cur_va_64 = 0;
    std::uint32_t cur_fo = 0;
    for (std::uint16_t i = 0; i < tgt_n; ++i) {
        const auto& s = tgt_secs[i];
        std::uint32_t v_size = s.Misc.VirtualSize > s.SizeOfRawData
                                ? s.Misc.VirtualSize : s.SizeOfRawData;
        std::uint64_t v_end = static_cast<std::uint64_t>(s.VirtualAddress) +
                              v_size;
        if (v_end > cur_va_64) cur_va_64 = v_end;
        std::uint32_t f_end = s.PointerToRawData + header_expansion +
                              s.SizeOfRawData;
        if (f_end > cur_fo) cur_fo = f_end;
    }
    if (old_symtab_offset != 0) {
        std::uint32_t f_end = old_symtab_offset + header_expansion +
                              old_symtab_size + old_strtab_size;
        if (f_end > cur_fo) cur_fo = f_end;
    }
    if (pe_in.size() + header_expansion > cur_fo) {
        cur_fo = static_cast<std::uint32_t>(pe_in.size()) + header_expansion;
    }
    std::uint32_t section_va = roundUp(static_cast<std::uint32_t>(cur_va_64),
                                       sect_align);
    std::uint32_t section_fo = roundUp(cur_fo, file_align);

    const std::uint32_t raw_size = roundUp(section_payload_size, file_align);

    const std::uint32_t final_symtab_offset = section_fo + raw_size;
    const std::uint32_t final_size = final_symtab_offset +
                                     old_symtab_size + old_strtab_size;

    // ---- Build output buffer ----
    pe_out.assign(final_size, 0);
    if (header_expansion == 0) {
        std::memcpy(pe_out.data(), pe_in.data(), pe_in.size());
    } else {
        std::memcpy(pe_out.data(), pe_in.data(), size_of_headers);
        std::memcpy(pe_out.data() + size_of_headers + header_expansion,
                    pe_in.data() + size_of_headers,
                    pe_in.size() - size_of_headers);
    }

    auto* out_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(
        pe_out.data() + dos->e_lfanew);
    auto* out_secs = reinterpret_cast<IMAGE_SECTION_HEADER*>(
        reinterpret_cast<std::uint8_t*>(out_nt)
            + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
            + out_nt->FileHeader.SizeOfOptionalHeader);

    if (header_expansion != 0) {
        for (std::uint16_t i = 0; i < tgt_n; ++i) {
            if (out_secs[i].PointerToRawData != 0) {
                out_secs[i].PointerToRawData += header_expansion;
            }
        }
        out_nt->OptionalHeader.SizeOfHeaders =
            size_of_headers + header_expansion;
        if (out_nt->FileHeader.PointerToSymbolTable != 0) {
            out_nt->FileHeader.PointerToSymbolTable += header_expansion;
        }
    }

    // Copy existing symtab + string table to its new (final) location.
    const std::uint32_t shifted_old_symtab_off =
        old_symtab_offset == 0 ? 0 : old_symtab_offset + header_expansion;
    if (shifted_old_symtab_off != 0 && old_symtab_size > 0) {
        std::memcpy(pe_out.data() + final_symtab_offset,
                    pe_out.data() + shifted_old_symtab_off,
                    old_symtab_size);
    }
    if (old_symtab_offset != 0) {
        std::memcpy(pe_out.data() + final_symtab_offset + old_symtab_size,
                    pe_in.data() + old_symtab_offset + old_symtab_size,
                    old_strtab_size);
    } else {
        std::memcpy(pe_out.data() + final_symtab_offset + old_symtab_size,
                    &old_strtab_size, 4);
    }

    // ---- Write the new ".debug" section header + payload ----
    auto& nh = out_secs[tgt_n];
    std::memset(&nh, 0, sizeof(nh));
    static_assert(sizeof(nh.Name) == 8, "PE section name is 8 bytes");
    std::memcpy(nh.Name, ".debug", 6);   // .debug + two trailing NULs
    nh.Misc.VirtualSize  = section_payload_size;
    nh.VirtualAddress    = section_va;
    nh.SizeOfRawData     = raw_size;
    nh.PointerToRawData  = section_fo;
    nh.Characteristics   =
        IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA;

    // IMAGE_DEBUG_DIRECTORY at section_fo
    auto* dbg = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(
        pe_out.data() + section_fo);
    std::memset(dbg, 0, sizeof(*dbg));
    dbg->Characteristics  = 0;
    dbg->TimeDateStamp    = 0;
    dbg->MajorVersion     = 0;
    dbg->MinorVersion     = 0;
    dbg->Type             = IMAGE_DEBUG_TYPE_CODEVIEW;  // 2
    dbg->SizeOfData       = rsds_size;
    dbg->AddressOfRawData = section_va + dbg_dir_size;
    dbg->PointerToRawData = section_fo + dbg_dir_size;

    // RSDS payload at section_fo + 28
    std::uint8_t* rsds = pe_out.data() + section_fo + dbg_dir_size;
    std::memcpy(rsds, "RSDS", 4);
    std::memcpy(rsds + 4, guid.data(), 16);
    std::memcpy(rsds + 20, &age, 4);
    std::memcpy(rsds + 24, pdb_basename.c_str(), pdb_basename.size() + 1);

    // ---- Patch DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG] ----
    auto& dd = out_nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    dd.VirtualAddress = section_va;
    dd.Size           = dbg_dir_size;

    // ---- Final header patches ----
    out_nt->FileHeader.NumberOfSections     = static_cast<WORD>(tgt_n + 1);
    out_nt->FileHeader.PointerToSymbolTable = final_symtab_offset;
    out_nt->OptionalHeader.SizeOfImage      =
        roundUp(section_va + section_payload_size, sect_align);
    out_nt->OptionalHeader.CheckSum         = 0;

    return true;
}

bool injectPdbReferenceFile(const std::string& input_exe,
                            const pdb::Guid& guid,
                            std::uint32_t age,
                            const std::string& pdb_basename,
                            const std::string& output_exe,
                            std::string& error_out) {
    std::vector<std::uint8_t> in;
    if (!readFile(input_exe, in, error_out)) return false;
    std::vector<std::uint8_t> out;
    if (!injectPdbReference(in, guid, age, pdb_basename, out, error_out)) {
        return false;
    }
    return writeFile(output_exe, out, error_out);
}

} // namespace rsm2pdb::pe
