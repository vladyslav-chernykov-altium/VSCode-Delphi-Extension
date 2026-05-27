#pragma once

// PDB writer (LLVM-backed). Emits a PDB containing:
//   - Info / DBI / TPI / IPI / GSI infrastructure
//   - A SectionMap derived from the caller-supplied COFF section list
//     (needed for the debugger to resolve (segment, offset) -> RVA)
//   - S_PUB32 public symbols (functions + data) in the publics stream
//
// The PDB's GUID + age are caller-supplied so the RSDS Debug Directory
// entry in the target PE matches what the debugger reads from the PDB.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rsm2pdb::pdb {

// 16-byte GUID in raw byte order; matches llvm::codeview::GUID layout
// and goes directly into the PE's RSDS payload.
using Guid = std::array<std::uint8_t, 16>;

// A CodeView public symbol. `segment` is the 1-based PE section index
// and `offset` is the byte offset within that section.
struct PublicSymbol {
    std::string name;
    std::uint16_t segment = 0;
    std::uint32_t offset  = 0;
    bool is_function      = false;   // sets PublicSymFlags::Function
};

// Subset of IMAGE_SECTION_HEADER fields the SectionMap needs. The
// emitter packs these into the on-disk SectionMap entries verbatim.
struct CoffSection {
    std::string   name;                  // up to 8 chars used
    std::uint32_t virtual_size        = 0;
    std::uint32_t virtual_address     = 0;
    std::uint32_t size_of_raw_data    = 0;
    std::uint32_t pointer_to_raw_data = 0;
    std::uint32_t characteristics     = 0;
};

// One parameter or local variable inside a function. Encoded as one
// of three CodeView shapes:
//   - register_id != 0    -> S_LOCAL + S_DEFRANGE_REGISTER (variable
//                            lives in CPU register for the whole proc;
//                            used for tiny Delphi methods whose
//                            args/Self never spill to stack).
//   - optimized_out=true  -> S_LOCAL with no defrange ("<optimized
//                            away>" in cppvsdbg).
//   - default             -> S_REGREL32 at RBP + offset.
struct ModuleLocal {
    std::string   name;
    std::int32_t  offset = 0;          // RBP-relative when register_id==0
    bool          is_param = false;    // informational; CV doesn't distinguish
    bool          optimized_out = false;
    std::uint16_t register_id = 0;     // CodeView RegisterId (0 = not in reg)
    // Width of the variable as observed in machine code / RSM. Maps to:
    //   1 -> UChar, 2 -> UShort, 4 -> UInt32, 8 -> UInt64,
    //   other N -> LF_ARRAY of UChar with count=N (TPI record),
    //   0 -> void* (8-byte hex), used when size couldn't be inferred.
    // Everything is treated as raw hex on purpose: until we ship a real
    // Pascal type map (M3 follow-up), surfacing accurate byte counts is
    // more useful than mislabelling things as Integer.
    std::uint32_t byte_size = 0;
};

// Function inside a Pascal compile unit. Emits as S_GPROC32 + S_END
// inside its module's symbol stream.
struct ModuleFunction {
    std::string   name;
    std::uint16_t segment = 0;       // 1-based PE section
    std::uint32_t offset  = 0;       // bytes within section
    std::uint32_t size    = 0;       // code size
    std::vector<ModuleLocal> locals; // params first, then locals
};

// One (line, code-offset) pair for the C13 line subsection.
// `offset` is section-relative (matches the function's CodeOffset).
struct ModuleLine {
    std::uint16_t segment = 0;
    std::uint32_t offset  = 0;
    std::uint32_t line    = 0;       // 1-based source line
};

// One source file's worth of line entries within a compile unit. A
// single Pascal unit can have multiple of these when {$INCLUDE} pulls
// extra .pas / .inc files into the same module.
struct ModuleSource {
    std::string             source_path;   // absolute path preferred
    std::vector<ModuleLine> lines;
};

// A Pascal compile unit. Becomes one DBI module with N source files,
// N functions, and a C13 line subsection per source.
struct Module {
    std::string name;                       // e.g. "App.Colors"
    std::vector<ModuleSource>   sources;    // empty -> no line info
    std::vector<ModuleFunction> functions;
};

struct PdbInputs {
    Guid          guid{};
    std::uint32_t age = 1;
    std::vector<PublicSymbol> publics;
    std::vector<CoffSection>  sections;
    std::vector<Module>       modules;
};

// Write a PDB. Returns true on success; fills `error_out` with the
// LLVM error description on failure.
bool writePdb(const std::string& path,
              const PdbInputs& inputs,
              std::string& error_out);

} // namespace rsm2pdb::pdb
