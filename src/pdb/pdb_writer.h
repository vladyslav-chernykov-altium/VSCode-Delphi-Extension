#pragma once

// PDB writer (LLVM-backed). Emits a PDB containing:
//   - Info / DBI / TPI / IPI / GSI infrastructure
//   - A SectionMap derived from the caller-supplied COFF section list
//     (needed for the debugger to resolve (segment, offset) -> RVA)
//   - S_PUB32 public symbols (functions + data) in the publics stream
//
// The PDB's GUID + age are caller-supplied so the RSDS Debug Directory
// entry in the target PE matches what the debugger reads from the PDB.

#include "model/model.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rsm2pdb::pdb {

// 16-byte GUID in raw byte order; matches llvm::codeview::GUID layout
// and goes directly into the PE's RSDS payload.
using Guid = std::array<std::uint8_t, 16>;

// One field of a user aggregate (Pascal `record`). PDB writer
// emits LF_FIELDLIST containing one LF_MEMBER per field, followed
// by a single LF_STRUCTURE record per aggregate (see PdbInputs::
// aggregates below). Field's TypeIndex is resolved from either
// `prim_kind` (primitives) or `nested_aggregate` (recursive record
// types like TBox.TopLeft : TPoint). When both are nullopt the
// writer falls back to a 1-byte UChar.
struct AggregateField {
    std::string   name;
    std::uint32_t byte_offset = 0;
    // Field width in bytes. Required for boolean / wide-int
    // disambiguation -- typeForKindParts picks Boolean8 vs Boolean32
    // (etc.) from this value. 0 = unknown; falls back to 4.
    std::uint32_t byte_size = 0;
    std::optional<model::PrimitiveKind>    prim_kind;
    // Index into PdbInputs::aggregates of the nested aggregate's
    // record (only set for composite-typed fields). Forward indices
    // are legal -- the writer emits LF_STRUCTUREs in dependency
    // order so the nested type is always registered first.
    std::optional<std::size_t>             nested_aggregate;
};

// A user-declared record / class. PDB writer emits one LF_STRUCTURE
// (CLASS_FLAGS plain record for now -- class/enum lifting comes in
// Phase E / F) per entry. Variable / public symbols reference it by
// index via `aggregate_index`.
struct AggregateRecord {
    std::string   name;
    std::uint32_t byte_size = 0;
    std::vector<AggregateField> fields;
};

// A CodeView public symbol. `segment` is the 1-based PE section index
// and `offset` is the byte offset within that section.
struct PublicSymbol {
    std::string name;
    std::uint16_t segment = 0;
    std::uint32_t offset  = 0;
    bool is_function      = false;   // sets PublicSymFlags::Function
    // For data symbols (is_function==false), the byte width + optional
    // Pascal primitive kind drive the S_GDATA32 TypeIndex selection
    // exactly like ModuleLocal below. Functions ignore both fields.
    std::uint32_t byte_size = 0;
    std::optional<model::PrimitiveKind> prim_kind;
    // When set, overrides prim_kind / byte_size for TypeIndex
    // selection -- the writer routes through the LF_STRUCTURE
    // emitted for this aggregate. Index into PdbInputs::aggregates.
    std::optional<std::size_t> aggregate_index;
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
    // Width of the variable as observed in machine code / RSM. When
    // `prim_kind` is unset (Pascal type not resolved), this drives an
    // unsigned-integer fallback chain:
    //   1 -> UChar, 2 -> UShort, 4 -> UInt32, 8 -> UInt64,
    //   other N -> LF_ARRAY of UChar with count=N (TPI record),
    //   0 -> void* (8-byte hex), used when size couldn't be inferred.
    std::uint32_t byte_size = 0;
    // Resolved Pascal primitive kind. When present overrides the
    // size-based mapping above so signed vs unsigned ints, integer vs
    // float of the same width, booleans, chars, etc. each get their
    // own CodeView simple type (Int32 vs UInt32 vs Real32, Bool8 vs
    // UChar, WChar vs UShort, ...). Resolved upstream from the RSM
    // per-unit type table; nullopt for variables that fall back to
    // pure size-based hex.
    std::optional<model::PrimitiveKind> prim_kind;
    // When set, overrides prim_kind / byte_size for TypeIndex
    // selection. Locals of record type (TPoint, TPerson, ...) get
    // routed to the LF_STRUCTURE the writer emits for the indexed
    // aggregate, so cdb / VS show them field-by-field instead of
    // as an opaque byte array. Index into PdbInputs::aggregates.
    std::optional<std::size_t> aggregate_index;
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
    // Aggregate types (records, eventually classes/enums) referenced
    // by `aggregate_index` on ModuleLocal / PublicSymbol. The writer
    // emits one LF_FIELDLIST + LF_STRUCTURE per entry into TPI.
    // Indices are stable across the whole PdbInputs; nested
    // references inside AggregateField::nested_aggregate point into
    // this same vector. Callers should populate in dependency order
    // (nested types before their containers) -- the writer doesn't
    // topologically sort.
    std::vector<AggregateRecord> aggregates;
};

// Write a PDB. Returns true on success; fills `error_out` with the
// LLVM error description on failure.
bool writePdb(const std::string& path,
              const PdbInputs& inputs,
              std::string& error_out);

} // namespace rsm2pdb::pdb
