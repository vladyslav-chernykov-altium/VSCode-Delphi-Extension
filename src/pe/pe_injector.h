#pragma once

#include "dwarf/dwarf_emitter.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rsm2pdb::pe {

// Inject DWARF section bytes into a PE (Win64) image.
//
// The injector:
//   - validates the input is a 64-bit PE (PE32+),
//   - reserves new section header slots, expanding SizeOfHeaders by
//     one FileAlignment unit if necessary (shifting existing section
//     data forward by the same amount),
//   - appends new section raw data past the existing data,
//   - registers each new section in the section header table using
//     the COFF "/N" string-table convention for long names,
//   - preserves any existing symbol table and extends the string
//     table with our long section names,
//   - patches NumberOfSections, SizeOfImage, CheckSum, and
//     PointerToSymbolTable.
//
// pe_in is consumed read-only; pe_out is overwritten with the new
// PE bytes. On failure, returns false and populates error_out.

bool injectDwarfSections(const std::vector<std::uint8_t>& pe_in,
                         const dwarf::DwarfSections& sections,
                         std::vector<std::uint8_t>& pe_out,
                         std::string& error_out);

// Convenience: read input.exe from disk, run injection, write
// output.exe. Same semantics as above plus file IO.
bool injectDwarfFile(const std::string& input_exe,
                     const dwarf::DwarfSections& sections,
                     const std::string& output_exe,
                     std::string& error_out);

} // namespace rsm2pdb::pe
