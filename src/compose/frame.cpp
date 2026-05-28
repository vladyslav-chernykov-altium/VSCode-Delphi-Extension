#include "compose/frame.h"

#include "pe/prologue.h"
#include "pe/size_sniffer.h"

#include <algorithm>
#include <unordered_set>

namespace rsm2pdb::compose {

bool isSelf(const rsm::Variable& v) {
    // The name check catches every Self regardless of marker (incl.
    // override-method Self with marker 0x31, which we don't list
    // explicitly because the per-unit type-marker palette can
    // theoretically reassign 0x31 to a primitive in a unit that
    // happens to use 25+ types). The marker checks pick up the cases
    // where the Pascal source uses an alias / fwd-declared `this`.
    return v.name == "Self"
        || v.type_marker == 0x29
        || v.type_marker == 0xD5;
}

std::unordered_map<std::uint8_t, std::uint32_t>
buildMarkerSizes(const rsm::Reader& reader) {
    std::unordered_map<std::uint8_t, std::uint32_t> result;
    if (reader.variables().empty()) return result;

    std::vector<std::uint64_t> all_vas;
    all_vas.reserve(reader.variables().size());
    for (const auto& v : reader.variables()) {
        if (v.is_primitive && v.address != 0) {
            all_vas.push_back(v.address);
        }
    }
    std::sort(all_vas.begin(), all_vas.end());
    all_vas.erase(std::unique(all_vas.begin(), all_vas.end()),
                  all_vas.end());

    std::unordered_set<std::uint8_t> conflicting;
    for (const auto& v : reader.variables()) {
        if (!v.is_primitive || v.address == 0) continue;
        auto it = std::upper_bound(all_vas.begin(), all_vas.end(),
                                   v.address);
        if (it == all_vas.end()) continue;
        const std::uint64_t gap = *it - v.address;
        // Gaps larger than a register-sized value mean we straddled
        // an unrelated symbol (or hit the end of a section). Skip
        // rather than overestimate.
        if (gap == 0 || gap > 16) continue;
        const auto size = static_cast<std::uint32_t>(gap);
        auto [hit, inserted] = result.emplace(v.type_marker, size);
        if (!inserted && hit->second != size) {
            conflicting.insert(v.type_marker);
        }
    }
    for (auto m : conflicting) result.erase(m);
    return result;
}

namespace {

// Width-resolution policy (see frame.h comments). Three-tier fallback:
//   1. Self markers force 8 (pointer to object / interface ref).
//   2. RSM's per-unit marker → size table (consistent across the
//      whole binary by construction).
//   3. Disasm-sniffed width at the variable's rbp-relative offset
//      (typically the prologue's spill instruction width).
// Returns 0 when no source resolves -- callers emit a void*-equiv.
std::uint32_t resolveSize(
    const rsm::Variable& v,
    std::int32_t         real_off,
    const std::unordered_map<std::uint8_t, std::uint32_t>& marker_sizes,
    const std::unordered_map<std::int32_t, std::uint32_t>& sniffed)
{
    if (isSelf(v)) return 8;
    if (auto it = marker_sizes.find(v.type_marker);
        it != marker_sizes.end())
    {
        return it->second;
    }
    if (auto it = sniffed.find(real_off); it != sniffed.end()) {
        return it->second;
    }
    return 0;
}

// Stamp a Pascal primitive kind + size onto an in-progress
// ResolvedVar when RSM resolved a type name for the source variable.
// Overrides whatever size came out of the byte-width fallback chain
// because the Pascal name is the most precise source we have.
void applyPascalType(ResolvedVar& rv, const rsm::Variable& v) {
    if (v.pascal_type.empty()) return;
    if (auto rp = rsm::Reader::resolvePrimitive(v.pascal_type)) {
        rv.prim_kind = rp->kind;
        rv.byte_size = rp->byte_size;
    }
}

} // namespace

ResolvedFunction resolveFunction(
    const rsm::ProcedureRecord& proc,
    const std::uint8_t*          code,
    std::size_t                  code_len,
    const std::unordered_map<std::uint8_t, std::uint32_t>& marker_sizes)
{
    ResolvedFunction out;
    const auto prologue = pe::parsePrologue(code, code_len);
    out.sub_rsp      = prologue.sub_rsp;
    out.extra_pushes = prologue.extra_pushes;

    // Each callee-saved push between `push rbp` and `sub rsp` lives
    // in the 8-byte slot immediately above the local area, so saved-
    // rbp / return-addr / rcx-shadow all shift up by 8 *
    // extra_pushes. Locals themselves (which sit at rbp + 0 ..
    // rbp + sub_rsp) don't move.
    const std::int32_t param_shift =
        8 * static_cast<std::int32_t>(out.extra_pushes);
    const auto sniffed = pe::sniffStackVarSizes(code, code_len);

    out.vars.reserve(
        (proc.has_static_link ? 1u : 0u)
        + proc.params.size() + proc.locals.size());

    // Pascal nested functions get an implicit static-link param
    // (pointer to outer's stack frame) in rcx; Delphi spills it to
    // the first shadow slot but the RSM record never mentions it.
    // Surface it as a synthesised __frame_outer__ var so consumers
    // can navigate to outer's locals.
    if (proc.has_static_link) {
        ResolvedVar sl;
        sl.name           = "__frame_outer__";
        sl.is_param       = true;
        sl.is_static_link = true;
        sl.rbp_offset     = out.sub_rsp + 16 + param_shift;
        sl.byte_size      = 8;
        out.vars.push_back(std::move(sl));
    }

    for (const auto& p : proc.params) {
        ResolvedVar rv;
        rv.name     = p.name;
        rv.is_param = true;
        rv.is_self  = isSelf(p);
        rv.rbp_offset = rv.is_self
                        ? out.sub_rsp + 16 + param_shift
                        : out.sub_rsp + param_shift
                              + (p.stack_offset / 2);
        rv.byte_size = resolveSize(p, rv.rbp_offset,
                                   marker_sizes, sniffed);
        applyPascalType(rv, p);
        out.vars.push_back(std::move(rv));
    }
    for (const auto& l : proc.locals) {
        ResolvedVar rv;
        rv.name     = l.name;
        rv.is_param = false;
        rv.rbp_offset = out.sub_rsp + (l.stack_offset / 2);
        rv.byte_size = resolveSize(l, rv.rbp_offset,
                                   marker_sizes, sniffed);
        applyPascalType(rv, l);
        out.vars.push_back(std::move(rv));
    }
    return out;
}

} // namespace rsm2pdb::compose
