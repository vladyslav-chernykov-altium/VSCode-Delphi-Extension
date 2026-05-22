#pragma once

#include "model/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rsm2pdb::dwarf {

// Produced sections, ready to be appended to a PE.
struct DwarfSections {
    std::vector<std::uint8_t> debug_info;
    std::vector<std::uint8_t> debug_abbrev;
    std::vector<std::uint8_t> debug_line;
    std::vector<std::uint8_t> debug_str;
    std::vector<std::uint8_t> debug_line_str;
};

struct EmitOptions {
    // Written verbatim into DW_AT_producer.
    std::string producer = "rsm2pdb 0.1";

    // 32-bit DWARF (4-byte offsets) or 64-bit DWARF (8-byte offsets).
    // We use 32-bit; data sizes are far below 4 GiB.
    // Reserved for future toggling. Not currently a knob.
};

// Emit DWARF v5 sections from a populated model::Module.
// Returns true on success; on failure, error_out describes the issue.
bool emit(const model::Module& mod,
          const EmitOptions& opts,
          DwarfSections& out,
          std::string& error_out);

} // namespace rsm2pdb::dwarf
