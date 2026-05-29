#include "rsm/rsm_reader.h"
#include "rsm/rsm_internal.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace rsm2pdb::rsm {

using namespace detail;

namespace {

// Heuristic mapping from byte-size to the most common Pascal
// primitive of that size. Loses Cardinal/Single (4 bytes) and
// UInt64/Double (8 bytes) distinction but covers the >95% case.
// Returns std::nullopt for non-standard sizes; the caller falls
// back to a `byte[N]` array so the displayed width matches the
// actual memory footprint.
std::optional<model::PrimitiveKind> primitiveKindForSize(std::uint64_t size) {
    switch (size) {
    case 1: return model::PrimitiveKind::UInt8;   // Byte
    case 2: return model::PrimitiveKind::UInt16;  // Word
    case 4: return model::PrimitiveKind::Int32;   // Integer
    case 8: return model::PrimitiveKind::Int64;   // Int64
    default: return std::nullopt;
    }
}

} // namespace

void decorateTypes(const Reader& reader, model::Module& mod) {
    // Collect all variable VAs across the whole module so we can
    // compute next-symbol-gap as a size estimate for each variable.
    std::vector<std::uint64_t> all_var_vas;
    all_var_vas.reserve(64);
    for (const auto& cu : mod.units) {
        for (const auto& s : cu.symbols) {
            if (s.kind == model::SymbolKind::Variable) {
                all_var_vas.push_back(s.address);
            }
        }
    }
    std::sort(all_var_vas.begin(), all_var_vas.end());
    all_var_vas.erase(std::unique(all_var_vas.begin(), all_var_vas.end()),
                      all_var_vas.end());

    auto nextHigher = [&](std::uint64_t va) -> std::uint64_t {
        auto it = std::upper_bound(all_var_vas.begin(), all_var_vas.end(), va);
        return it == all_var_vas.end() ? 0 : *it;
    };

    // Memoize "byte[N]" array types so identical fallbacks share a TypeId.
    // We need a single underlying `byte` base type; create it on first use.
    model::TypeId byte_type_id = model::kNoType;
    auto byteType = [&]() {
        if (byte_type_id == model::kNoType) {
            byte_type_id = mod.addType(
                model::Type{model::PrimitiveType{model::PrimitiveKind::UInt8}});
        }
        return byte_type_id;
    };
    std::map<std::uint64_t, model::TypeId> byte_array_cache;
    auto byteArrayOfSize = [&](std::uint64_t n) {
        auto it = byte_array_cache.find(n);
        if (it != byte_array_cache.end()) return it->second;
        model::TypeId tid = mod.addType(
            model::Type{model::ArrayType{byteType(), n}});
        byte_array_cache[n] = tid;
        return tid;
    };

    // Process each compile unit in turn. Markers are unit-local, so
    // the marker-grouping happens per CU.
    for (auto& cu : mod.units) {
        // Group primitive variables by their type_marker; collect
        // non-primitive variables separately.
        std::map<std::uint8_t, std::vector<model::Symbol*>> by_marker;
        std::vector<model::Symbol*> non_primitives;

        for (auto& s : cu.symbols) {
            if (s.kind != model::SymbolKind::Variable) continue;
            const auto* v = reader.findVariableAt(s.address);
            if (!v) continue;
            if (v->is_primitive) {
                by_marker[v->type_marker].push_back(&s);
            } else {
                non_primitives.push_back(&s);
            }
        }

        // Primitive marker groups: one PrimitiveType per marker.
        // For non-standard sizes (when alignment padding inflates the
        // gap past a primitive width) fall back to a byte[N] array.
        for (auto& [marker, syms] : by_marker) {
            std::uint64_t min_gap = 0;
            for (auto* s : syms) {
                const auto next = nextHigher(s->address);
                if (next > s->address) {
                    const auto gap = next - s->address;
                    if (min_gap == 0 || gap < min_gap) min_gap = gap;
                }
            }
            if (min_gap == 0) min_gap = 4;  // last symbol; assume Integer

            model::TypeId tid;
            if (auto kind = primitiveKindForSize(min_gap)) {
                tid = mod.addType(model::Type{model::PrimitiveType{*kind}});
            } else {
                tid = byteArrayOfSize(min_gap);
            }
            for (auto* s : syms) s->type = tid;
        }

        // Non-primitive variables: byte[N] fallback sized by next-gap.
        // The aggregate-aware path lives in the PDB pipeline directly
        // (Phase D); the DWARF emitter still needs its array-of-byte
        // fallback here until Phase J takes it over.
        for (auto* s : non_primitives) {
            const auto next = nextHigher(s->address);
            std::uint64_t n = (next > s->address) ? (next - s->address) : 1;
            if (n == 0) n = 1;
            s->type = byteArrayOfSize(n);
        }

        // -- Procedure params + locals -------------------------------------
        //
        // For each function Symbol in this CU, look up its procedure
        // record in the RSM by VA. Then map each param/local's per-unit
        // type_marker -> TypeId, reusing the same marker->type table we
        // built for globals where available. For markers not seen among
        // globals (e.g. Geometry has only procedures, no globals at all)
        // fall back to Integer (4-byte signed) for primitives and
        // byte[8] for non-primitives.
        //
        // Build a marker->TypeId map from the just-processed globals.
        std::map<std::uint8_t, model::TypeId> marker_to_type;
        for (const auto& [marker, syms] : by_marker) {
            if (!syms.empty()) marker_to_type[marker] = syms.front()->type;
        }
        model::TypeId default_primitive_id = model::kNoType;
        auto integerType = [&]() {
            if (default_primitive_id == model::kNoType) {
                default_primitive_id = mod.addType(
                    model::Type{model::PrimitiveType{model::PrimitiveKind::Int32}});
            }
            return default_primitive_id;
        };

        auto resolveParamLocalType = [&](const Variable& v) -> model::TypeId {
            if (v.is_primitive) {
                auto it = marker_to_type.find(v.type_marker);
                if (it != marker_to_type.end()) return it->second;
                return integerType();   // unit has no global of this marker
            } else {
                // Records / classes / enums: byte[N] fallback. We don't
                // know N for params/locals at this stage -- default to
                // a sensible 8 bytes which fits TPoint and most small
                // records. PDB pipeline (Phase D) has its own struct
                // synth that goes via PdbInputs::aggregates; DWARF
                // will gain it in Phase J.
                return byteArrayOfSize(8);
            }
        };

        for (auto& s : cu.symbols) {
            if (s.kind != model::SymbolKind::Function) continue;
            const auto* proc = reader.findProcedureAt(s.address);
            if (!proc) continue;

            s.params.reserve(proc->params.size());
            for (const auto& pv : proc->params) {
                model::LocalVar lv;
                lv.name         = pv.name;
                lv.type         = resolveParamLocalType(pv);
                lv.stack_offset = pv.stack_offset;
                s.params.push_back(std::move(lv));
            }
            s.locals.reserve(proc->locals.size());
            for (const auto& lvar : proc->locals) {
                model::LocalVar lv;
                lv.name         = lvar.name;
                lv.type         = resolveParamLocalType(lvar);
                lv.stack_offset = lvar.stack_offset;
                s.locals.push_back(std::move(lv));
            }
        }
    }
}

} // namespace rsm2pdb::rsm
