#pragma once

// PE injector for the RSDS / CodeView Debug Directory entry. Adds one
// new section to the target PE containing:
//
//   [IMAGE_DEBUG_DIRECTORY (28B)] [RSDS payload]
//
// and points OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]
// at the IMAGE_DEBUG_DIRECTORY.
//
// The RSDS payload is the standard CV_INFO_PDB70 layout:
//   "RSDS"  (4B)
//   GUID    (16B, raw bytes -- same order as llvm::codeview::GUID)
//   Age     (u32 LE)
//   PDB path (UTF-8, NUL-terminated)
//
// The injector reuses the same header-room expansion mechanics as the
// DWARF injector (pe_injector.cpp) and is intended to coexist with it
// -- a PE may carry both DWARF sections (for gdb/lldb) and an RSDS
// pointer (for Windows debuggers like WinDbg / VS).

#include "pdb/pdb_writer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rsm2pdb::pe {

bool injectPdbReference(const std::vector<std::uint8_t>& pe_in,
                        const pdb::Guid& guid,
                        std::uint32_t age,
                        const std::string& pdb_basename,
                        std::vector<std::uint8_t>& pe_out,
                        std::string& error_out);

bool injectPdbReferenceFile(const std::string& input_exe,
                            const pdb::Guid& guid,
                            std::uint32_t age,
                            const std::string& pdb_basename,
                            const std::string& output_exe,
                            std::string& error_out);

} // namespace rsm2pdb::pe
