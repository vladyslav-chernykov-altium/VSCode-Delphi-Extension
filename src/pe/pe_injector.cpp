// PE (Win64) injector: adds .debug_* sections to an existing PE.
//
// Ported from the spike/inject_files.c PoC. Same algorithm; cleaner
// C++ API. The wire-level behavior is bit-for-bit identical to the
// spike on inputs the spike could handle, plus the spike's expand-
// SizeOfHeaders extension.
//
// We deliberately do NOT use llvm::object::COFFObjectFile here. The
// PE manipulation we need is mechanical byte rewriting; an mmap+
// patch model is easier to reason about and keeps LLVM out of the
// production critical path for the injector.

#include "pe/pe_injector.h"

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

constexpr std::size_t kMaxInject = 8;

struct SectionEntry {
    std::string name;            // ".debug_info" etc.
    const std::vector<std::uint8_t>* data;
    std::uint32_t strtab_offset = 0;
};

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


bool injectDwarfSections(const std::vector<std::uint8_t>& pe_in,
                         const dwarf::DwarfSections& sections,
                         std::vector<std::uint8_t>& pe_out,
                         std::string& error_out) {
    // ---- Collect non-empty sections to inject ----
    SectionEntry entries[kMaxInject];
    std::size_t n_entries = 0;

    auto add = [&](const char* name, const std::vector<std::uint8_t>& d) {
        if (d.empty()) return;
        if (n_entries >= kMaxInject) return;
        entries[n_entries].name = name;
        entries[n_entries].data = &d;
        ++n_entries;
    };
    add(".debug_abbrev",   sections.debug_abbrev);
    add(".debug_str",      sections.debug_str);
    add(".debug_line_str", sections.debug_line_str);
    add(".debug_line",     sections.debug_line);
    add(".debug_info",     sections.debug_info);

    if (n_entries == 0) {
        error_out = "no DWARF sections to inject (all empty)";
        return false;
    }

    if (pe_in.size() < sizeof(IMAGE_DOS_HEADER)) {
        error_out = "input too small to be a PE";
        return false;
    }

    // ---- Parse input PE ----
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

    // ---- Header room: expand if needed ----
    const std::uint32_t sect_table_end_off =
        static_cast<std::uint32_t>(
            reinterpret_cast<const std::uint8_t*>(tgt_secs + tgt_n) -
            pe_in.data());
    const std::uint32_t header_free  = size_of_headers - sect_table_end_off;
    const std::uint32_t header_need  =
        static_cast<std::uint32_t>(n_entries) * sizeof(IMAGE_SECTION_HEADER);
    std::uint32_t header_expansion = 0;
    if (header_need > header_free) {
        header_expansion = roundUp(header_need - header_free, file_align);
    }

    // ---- Existing symbol + string table (preserve and extend) ----
    const std::uint32_t old_symtab_offset = nt->FileHeader.PointerToSymbolTable;
    const std::uint32_t old_n_symbols    = nt->FileHeader.NumberOfSymbols;
    const std::uint32_t old_symtab_size  = old_n_symbols * 18u;
    std::uint32_t old_strtab_size = 4;  // size field present even when empty
    if (old_symtab_offset != 0) {
        if (old_symtab_offset + old_symtab_size + 4 > pe_in.size()) {
            error_out = "symbol table out of bounds";
            return false;
        }
        std::memcpy(&old_strtab_size,
                    pe_in.data() + old_symtab_offset + old_symtab_size, 4);
    }

    // ---- Build new string table (old strings + our section names) ----
    std::uint32_t new_strtab_size = old_strtab_size;
    for (std::size_t i = 0; i < n_entries; ++i) {
        entries[i].strtab_offset = new_strtab_size;
        new_strtab_size += static_cast<std::uint32_t>(entries[i].name.size()) + 1u;
    }
    std::vector<std::uint8_t> new_strtab(new_strtab_size, 0);
    if (old_symtab_offset != 0) {
        std::memcpy(new_strtab.data(),
                    pe_in.data() + old_symtab_offset + old_symtab_size,
                    old_strtab_size);
    } else {
        std::memcpy(new_strtab.data(), &old_strtab_size, 4);
    }
    std::memcpy(new_strtab.data(), &new_strtab_size, 4);  // overwrite size
    for (std::size_t i = 0; i < n_entries; ++i) {
        std::memcpy(new_strtab.data() + entries[i].strtab_offset,
                    entries[i].name.data(),
                    entries[i].name.size() + 1);
    }

    // ---- Compute final file layout ----
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
    std::uint32_t cur_va = roundUp(static_cast<std::uint32_t>(cur_va_64),
                                   sect_align);
    cur_fo = roundUp(cur_fo, file_align);

    const std::uint32_t section_data_start = cur_fo;
    std::uint32_t section_data_end = cur_fo;
    for (std::size_t i = 0; i < n_entries; ++i) {
        section_data_end += roundUp(
            static_cast<std::uint32_t>(entries[i].data->size()), file_align);
    }
    const std::uint32_t final_symtab_offset = section_data_end;
    const std::uint32_t final_size = section_data_end +
                                     old_symtab_size + new_strtab_size;

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

    // If the input PE already carries pre-existing .debug_* sections
    // (from a previous rsm2pdb run, or built-in TD32 debug info),
    // rename them in the output so the loader / gdb doesn't accidentally
    // read the stale data instead of the freshly-injected sections.
    // Without this, gdb reads the FIRST .debug_info it finds, which is
    // the stale one, and reports e.g. "no locals or arguments" even
    // when our new DWARF correctly carries them.
    auto resolveSectionName = [&](const IMAGE_SECTION_HEADER& sh) -> std::string {
        if (sh.Name[0] != '/') {
            char buf[9] = {};
            std::memcpy(buf, sh.Name, 8);
            return std::string(buf);
        }
        // Long name: "/N" -> offset N into COFF string table.
        char numbuf[8] = {};
        int nlen = 0;
        for (int k = 1; k < 8 && sh.Name[k]; ++k) {
            numbuf[nlen++] = static_cast<char>(sh.Name[k]);
        }
        if (nlen == 0 || old_symtab_offset == 0) return {};
        std::uint32_t offset = static_cast<std::uint32_t>(
            std::strtoul(numbuf, nullptr, 10));
        if (offset >= old_strtab_size) return {};
        const std::uint8_t* strtab_base =
            pe_in.data() + old_symtab_offset + old_symtab_size;
        const char* p = reinterpret_cast<const char*>(strtab_base + offset);
        const std::size_t avail = old_strtab_size - offset;
        std::size_t len = 0;
        while (len < avail && p[len] != '\0') ++len;
        return std::string(p, len);
    };
    std::size_t renamed_debug = 0;
    for (std::uint16_t i = 0; i < tgt_n; ++i) {
        const auto name = resolveSectionName(tgt_secs[i]);
        if (name.size() >= 6 && name.compare(0, 6, ".debug") == 0) {
            std::memset(out_secs[i].Name, 0, 8);
            std::memcpy(out_secs[i].Name, ".old_dw", 7);
            ++renamed_debug;
        }
    }
    if (renamed_debug > 0) {
        std::fprintf(stderr,
                     "[pe-inject] renamed %zu pre-existing .debug_* section(s) to .old_dw\n",
                     renamed_debug);
    }

    // Copy existing symtab to its new (final) location, then write
    // the extended string table after it.
    const std::uint32_t shifted_old_symtab_off =
        old_symtab_offset == 0 ? 0 : old_symtab_offset + header_expansion;
    if (shifted_old_symtab_off != 0 && old_symtab_size > 0) {
        std::memcpy(pe_out.data() + final_symtab_offset,
                    pe_out.data() + shifted_old_symtab_off,
                    old_symtab_size);
    }
    std::memcpy(pe_out.data() + final_symtab_offset + old_symtab_size,
                new_strtab.data(),
                new_strtab.size());

    // Write section data + headers
    cur_fo = section_data_start;
    for (std::size_t i = 0; i < n_entries; ++i) {
        auto& nh = out_secs[tgt_n + i];
        std::memset(&nh, 0, sizeof(nh));

        char namefield[16] = {};
        std::snprintf(namefield, sizeof(namefield),
                      "/%u", entries[i].strtab_offset);
        std::memset(nh.Name, 0, 8);
        std::memcpy(nh.Name, namefield,
                    std::min<std::size_t>(8, std::strlen(namefield)));

        nh.Misc.VirtualSize  = static_cast<DWORD>(entries[i].data->size());
        nh.VirtualAddress    = cur_va;
        nh.SizeOfRawData     = roundUp(
            static_cast<std::uint32_t>(entries[i].data->size()), file_align);
        nh.PointerToRawData  = cur_fo;
        nh.Characteristics   =
            IMAGE_SCN_MEM_READ |
            IMAGE_SCN_CNT_INITIALIZED_DATA |
            IMAGE_SCN_MEM_DISCARDABLE;

        std::memcpy(pe_out.data() + cur_fo,
                    entries[i].data->data(),
                    entries[i].data->size());

        cur_va = roundUp(cur_va +
            static_cast<std::uint32_t>(entries[i].data->size()), sect_align);
        cur_fo = roundUp(cur_fo +
            static_cast<std::uint32_t>(entries[i].data->size()), file_align);
    }

    // Final header patches.
    out_nt->FileHeader.NumberOfSections =
        static_cast<WORD>(tgt_n + n_entries);
    out_nt->FileHeader.PointerToSymbolTable = final_symtab_offset;
    out_nt->OptionalHeader.SizeOfImage      = cur_va;
    out_nt->OptionalHeader.CheckSum         = 0;

    return true;
}


bool injectDwarfFile(const std::string& input_exe,
                     const dwarf::DwarfSections& sections,
                     const std::string& output_exe,
                     std::string& error_out) {
    std::vector<std::uint8_t> pe_in;
    if (!readFile(input_exe, pe_in, error_out)) return false;
    std::vector<std::uint8_t> pe_out;
    if (!injectDwarfSections(pe_in, sections, pe_out, error_out)) return false;
    if (!writeFile(output_exe, pe_out, error_out)) return false;
    return true;
}

} // namespace rsm2pdb::pe
