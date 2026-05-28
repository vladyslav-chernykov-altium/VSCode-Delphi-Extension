#pragma once

// Delphi-x64 stack-frame resolver. Takes a `rsm::ProcedureRecord` plus
// the function's PE bytes and produces a `ResolvedFunction` carrying
// every variable's rbp-relative offset, byte size, and Pascal
// primitive kind (when resolvable). This is the single source-of-
// truth interpretation of how RSM offsets map onto Delphi's modern
// x64 frame layout -- the formulas (sub_rsp + RSM/2 for locals,
// + 8*extra_pushes shift for params/Self, etc.) live here, not in
// each emitter.
//
// Both the PDB and DWARF backends consume ResolvedFunction. The PDB
// path takes `prim_kind` into account when picking a CodeView
// SimpleTypeKind; the DWARF path passes it through to
// describePrimitive(). Backends are free to ignore fields they don't
// understand.

#include "model/model.h"
#include "rsm/rsm_reader.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rsm2pdb::compose {

struct ResolvedVar {
    std::string                          name;
    // Signed RBP-relative offset of the variable's slot. For locals
    // this is negative-ish (rbp + 0 .. rbp + sub_rsp); for params /
    // Self it's typically rbp + sub_rsp + 16 + 8*extra_pushes
    // upwards.
    std::int32_t                         rbp_offset = 0;
    // 0 means "unknown size" -- consumers should fall back to a
    // pointer-sized hex representation (void* in PDB / 8-byte addr
    // in DWARF).
    std::uint32_t                        byte_size = 0;
    // Pascal primitive kind resolved through the per-unit type
    // table; nullopt when the marker didn't decode (no anchor,
    // marker > table size, RTL unit we don't parse, ...).
    std::optional<model::PrimitiveKind>  prim_kind;
    bool                                 is_param = false;
    bool                                 is_self  = false;
    // True for the synthesised `__frame_outer__` placeholder
    // emitted for nested-function records (RSM subtag 0x41). The
    // slot holds the parent function's rbp (passed in rcx and
    // spilled to the first shadow slot).
    bool                                 is_static_link = false;
};

struct ResolvedFunction {
    // Immediate from `sub rsp, N` in the function's prologue
    // (0 when the parser couldn't recognise the prologue shape, in
    // which case all rbp_offsets degrade to raw RSM/2 numbers).
    std::int32_t              sub_rsp = 0;
    // Number of callee-saved register pushes between `push rbp` and
    // `sub rsp` -- shifts the shadow-store area up by 8 each.
    std::uint8_t              extra_pushes = 0;
    // Resolved variables in emission order: synthesised
    // __frame_outer__ first (if present), then params, then locals.
    std::vector<ResolvedVar>  vars;
};

// Mark a variable as Self based on both the canonical Pascal name
// and the non-primitive markers RSM tags Self with. Exposed because
// the PDB globals-typing pass uses it too (Self never appears as a
// global, but the same predicate keeps the call sites symmetrical).
bool isSelf(const rsm::Variable& v);

// One-shot pass: collect every primitive global's (marker, size) and
// keep markers whose size resolves consistently across all globals
// that use them. Markers with conflicting sizes get dropped --
// callers should not infer anything from their absence in the map.
// The result is reused as one of the three size-resolution sources
// inside resolveFunction (Pascal type > this map > disasm sniff >
// unknown).
std::unordered_map<std::uint8_t, std::uint32_t>
buildMarkerSizes(const rsm::Reader& reader);

// Resolve a single procedure's stack-frame layout. `code` /
// `code_len` give read access to the function's bytes inside the
// mapped PE; pass {nullptr, 0} if the function couldn't be located
// (resolver degrades to sub_rsp = 0, extra_pushes = 0).
ResolvedFunction resolveFunction(
    const rsm::ProcedureRecord& proc,
    const std::uint8_t*          code,
    std::size_t                  code_len,
    const std::unordered_map<std::uint8_t, std::uint32_t>& marker_sizes);

} // namespace rsm2pdb::compose
