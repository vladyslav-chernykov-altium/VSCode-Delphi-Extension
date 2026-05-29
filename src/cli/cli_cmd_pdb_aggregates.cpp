// cli_cmd_pdb_aggregates.cpp -- aggregate registrar, Self placeholders,
// in-proc set synthesis, globals-via-RSM enrichment, and the RTL-name
// predicate. Part of the cli_cmd_pdb split.

#include "cli/cli.h"
#include "cli/cli_cmd_pdb_internal.h"

#include "cli/source_path.h"
#include "cli/util.h"
#include "compose/frame.h"
#include "map/map_reader.h"
#include "pdb/pdb_writer.h"
#include "pe/pe_pdb_injector.h"
#include "pe/thunk_scanner.h"
#include "rsm/rsm_reader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rsm2pdb::cli::pdb_detail {

bool isRtlQName(std::string_view q) {
  static constexpr std::string_view roots[] = {
      "System", "SysInit", "Winapi", "Vcl",  "Fmx", "Soap",       "Datasnap",
      "Data",   "Inet",    "IBX",    "REST", "Bde", "IndySystem",
  };
  for (auto r : roots) {
    if (q.size() < r.size())
      continue;
    if (q.substr(0, r.size()) != r)
      continue;
    if (q.size() == r.size())
      return true;
    if (q[r.size()] == '.')
      return true;
  }
  return false;
}

std::optional<std::size_t>
Context::registerAggr(const rsm2pdb::rsm::AggregateType *a) {
  auto &inputs = this->inputs;
  auto &rsm_reader = this->rsm_reader;
  auto &aggr_idx_cache = this->aggr_idx_cache;
  auto &aggr_in_progress = this->aggr_in_progress;
  auto &self_opaque_cache = this->self_opaque_cache;
  auto &inproc_set_cache = this->inproc_set_cache;
  auto &agg_by_name = this->agg_by_name;
  auto &agg_by_unit_name = this->agg_by_unit_name;
  auto registerAggr = [this](const rsm2pdb::rsm::AggregateType *a) {
    return this->registerAggr(a);
  };

  if (a == nullptr)
    return std::nullopt;
  const bool is_record = a->kind == rsm2pdb::rsm::AggregateKind::Record ||
                         a->kind == rsm2pdb::rsm::AggregateKind::PackedRecord;
  const bool is_class = a->kind == rsm2pdb::rsm::AggregateKind::Class;
  const bool is_enum = a->kind == rsm2pdb::rsm::AggregateKind::Enum;

  // Set detection -- RSM doesn't carry an explicit "set of X"
  // marker on the type record, so we infer from a name-suffix
  // heuristic: an aggregate with kind Unknown (no fields, no
  // enum entries, no class header) whose name is a registered
  // Enum's name plus a trailing `s`. Hits TColors -> TColor,
  // TKeys -> TKey, etc. False positives are rare because the
  // base-enum lookup must succeed in the SAME unit.
  const rsm2pdb::rsm::AggregateType *set_base = nullptr;
  if (a->kind == rsm2pdb::rsm::AggregateKind::Unknown && a->name.size() > 1 &&
      a->name.back() == 's') {
    const std::string base_name(a->name.data(), a->name.size() - 1);
    for (const auto &cand : rsm_reader.aggregates()) {
      if (cand.unit_anchor_offset != a->unit_anchor_offset)
        continue;
      if (cand.kind != rsm2pdb::rsm::AggregateKind::Enum)
        continue;
      if (cand.name != base_name)
        continue;
      set_base = &cand;
      break;
    }
  }
  const bool is_set = (set_base != nullptr);

  if (!is_record && !is_class && !is_enum && !is_set) {
    // Sets we couldn't pin down + Unknowns -- still on the
    // byte[N] fallback.
    return std::nullopt;
  }
  const std::pair<std::uint64_t, std::uint16_t> key{a->unit_anchor_offset,
                                                    a->own_hash};
  if (auto it = aggr_idx_cache.find(key); it != aggr_idx_cache.end()) {
    return it->second;
  }
  // Cycle break: if we're already registering this aggregate
  // (deeper in the call stack), bail out -- treat the
  // recursive reference as "unresolved". The writer's
  // dependency-order invariant (children emitted before
  // referrers) requires that registerAggr push to
  // inputs.aggregates AFTER recursing on children, so we
  // can't reserve a slot upfront without breaking that
  // invariant -- this is the cheapest workable alternative.
  if (!aggr_in_progress.insert(key).second) {
    return std::nullopt;
  }
  // For classes, recurse on the base class FIRST so its
  // index is registered before our own. The recursion may
  // return nullopt if it hits a cycle -- we treat that as
  // "no explicit base" (CodeView LF_BCLASS chains don't
  // cycle anyway).
  std::optional<std::size_t> base_idx;
  if (is_class && a->base_hash != 0) {
    const rsm2pdb::rsm::AggregateType *base_a =
        rsm_reader.findAggregateInUnit(a->unit_anchor_offset, a->base_hash);
    if (base_a == nullptr) {
      base_a = rsm_reader.findAggregateByHash(a->base_hash);
    }
    base_idx = registerAggr(base_a);
  }
  // NB: do NOT cache idx before recursing on fields -- the recursion
  // may push children to inputs.aggregates first (children-before-
  // referrers is the writer's TPI dependency-order invariant), making
  // any pre-recursion `inputs.aggregates.size()` snapshot stale by
  // push time. We cache idx AFTER push_back at each branch's tail
  // (set / enum / record-class). Self-references during recursion are
  // caught by aggr_in_progress (cycle break) which is checked above
  // BEFORE the cache lookup -- so a missing pre-recursion cache entry
  // is safe.
  rsm2pdb::pdb::AggregateRecord rec;
  rec.kind = is_class  ? rsm2pdb::pdb::AggregateKind::Class
             : is_enum ? rsm2pdb::pdb::AggregateKind::Enum
             : is_set  ? rsm2pdb::pdb::AggregateKind::Set
                       : rsm2pdb::pdb::AggregateKind::Record;
  rec.name = a->name;
  rec.byte_size = a->total_size;
  rec.base = base_idx;
  if (is_set) {
    // Borrow the base enum's enumerator names (one bit each).
    // Set width: Delphi rounds to the smallest power-of-two
    // byte count that fits all enumerators; floor at 1 byte.
    // For 4-value TColor we get 1 byte; for 33+ enumerators
    // it'd be 8 bytes. AdvPCB rarely needs >4.
    rec.enumerators.reserve(set_base->enum_entries.size());
    std::int64_t max_ord = 0;
    for (const auto &e : set_base->enum_entries) {
      rsm2pdb::pdb::AggregateEnumerator ae;
      ae.name = e.name;
      ae.value = e.ordinal;
      rec.enumerators.push_back(std::move(ae));
      if (e.ordinal > max_ord)
        max_ord = e.ordinal;
    }
    rec.byte_size = max_ord <= 7    ? 1
                    : max_ord <= 15 ? 2
                    : max_ord <= 31 ? 4
                                    : 8;
    inputs.aggregates.push_back(std::move(rec));
    const auto idx = inputs.aggregates.size() - 1;
    aggr_idx_cache[key] = idx;
    aggr_in_progress.erase(key);
    return idx;
  }
  if (is_enum) {
    // Enum size follows Delphi's default rule: max ordinal
    // <= 0xFF -> 1 byte, <= 0xFFFF -> 2 bytes, else 4.
    // {$MINENUMSIZE} can override; we don't model it yet.
    std::int64_t max_ord = 0;
    for (const auto &e : a->enum_entries) {
      if (e.ordinal > max_ord)
        max_ord = e.ordinal;
    }
    rec.byte_size = max_ord <= 0xFF ? 1 : max_ord <= 0xFFFF ? 2 : 4;
    rec.enumerators.reserve(a->enum_entries.size());
    for (const auto &e : a->enum_entries) {
      rsm2pdb::pdb::AggregateEnumerator ae;
      ae.name = e.name;
      ae.value = e.ordinal;
      rec.enumerators.push_back(std::move(ae));
    }
    inputs.aggregates.push_back(std::move(rec));
    const auto idx = inputs.aggregates.size() - 1;
    aggr_idx_cache[key] = idx;
    aggr_in_progress.erase(key);
    return idx;
  }
  rec.fields.reserve(a->fields.size());
  for (const auto &fe : a->fields) {
    rsm2pdb::pdb::AggregateField f;
    f.name = fe.name;
    f.byte_offset = fe.offset;
    if (fe.type_hash != 0) {
      // Composite field. Same-unit first, then last-wins.
      const rsm2pdb::rsm::AggregateType *sub =
          rsm_reader.findAggregateInUnit(a->unit_anchor_offset, fe.type_hash);
      if (sub == nullptr) {
        sub = rsm_reader.findAggregateByHash(fe.type_hash);
      }
      f.nested_aggregate = registerAggr(sub);
    } else if (fe.primitive_marker != 0) {
      // Phase X.2: a "primitive marker" can mean one of
      // three things:
      //   1. A real Pascal primitive (Integer, string, ...)
      //      whose name resolves via resolvePrimitive.
      //   2. A CROSS-UNIT type reference (TPoint, TItem, ...)
      //      whose name + 4-byte hash come from the
      //      enclosing unit's 0x66 table. To render
      //      these as proper records / classes we have
      //      to walk back to the DECLARING unit's
      //      aggregate via the 4-byte hash (low 16 bits
      //      = own_hash in declaring unit).
      //   3. An RTL-only type we don't decode -- falls
      //      through to UChar.
      if (const auto *entry = rsm_reader.unitTypeEntryForMarker(
              a->unit_anchor_offset, fe.primitive_marker)) {
        if (auto rp = rsm2pdb::rsm::Reader::resolvePrimitive(entry->name)) {
          // Case 1: real primitive.
          f.prim_kind = rp->kind;
          f.byte_size = rp->byte_size;
        } else if (entry->hash4 != 0) {
          // Case 2: cross-unit composite. Look up
          // declaring aggregate by (name, low 16
          // bits of hash4). agg_by_name was built
          // earlier (Phase E Self synth) -- reuse.
          const std::uint16_t target_own = entry->hash4 & 0xFFFFu;
          const rsm2pdb::rsm::AggregateType *foreign = nullptr;
          auto r = agg_by_name.equal_range(entry->name);
          for (auto it = r.first; it != r.second && !foreign; ++it) {
            if (it->second->own_hash == target_own) {
              foreign = it->second;
            }
          }
          // Pass 2: any aggregate with the name
          // (low-16-of-hash4 didn't match -- maybe
          // because the foreign aggregate's own_hash
          // was overwritten by chained re-indexing).
          if (foreign == nullptr && r.first != r.second) {
            foreign = r.first->second;
          }
          if (foreign != nullptr) {
            f.nested_aggregate = registerAggr(foreign);
          }
        }
      }
      // f.prim_kind stays nullopt when nothing resolves;
      // writer falls back to UChar.
    }
    rec.fields.push_back(std::move(f));
  }
  // Tier 2: copy parsed Pascal property names through to the PDB
  // aggregate record. The NatVis emitter consumes them; the PDB
  // itself ignores them entirely (CodeView has no representation).
  rec.property_names.reserve(a->properties.size());
  for (const auto& pe : a->properties) {
    rec.property_names.push_back(pe.name);
  }
  inputs.aggregates.push_back(std::move(rec));
  const auto idx = inputs.aggregates.size() - 1;
  aggr_idx_cache[key] = idx;
  aggr_in_progress.erase(key);
  return idx;
}

std::optional<std::size_t>
Context::lookupAggrIdx(const rsm2pdb::rsm::Variable &v) {
  if (v.is_primitive || v.inline_type_id == 0)
    return std::nullopt;
  const rsm2pdb::rsm::AggregateType *a = nullptr;
  if (v.unit_anchor_offset != 0) {
    a = rsm_reader.findAggregateInUnit(v.unit_anchor_offset, v.inline_type_id);
  }
  if (a == nullptr) {
    a = rsm_reader.findAggregateByHash(v.inline_type_id);
  }
  return registerAggr(a);
}

void enrichGlobalsViaRsm(Context &ctx) {
  auto &inputs = ctx.inputs;
  auto &rsm_reader = ctx.rsm_reader;
  const bool have_rsm = ctx.have_rsm;
  const auto image_base = ctx.image_base;
  auto &aggr_idx_cache = ctx.aggr_idx_cache;
  auto &aggr_in_progress = ctx.aggr_in_progress;
  auto &self_opaque_cache = ctx.self_opaque_cache;
  auto &inproc_set_cache = ctx.inproc_set_cache;
  auto &agg_by_name = ctx.agg_by_name;
  auto &agg_by_unit_name = ctx.agg_by_unit_name;
  // The registerAggr / lookupAggrIdx free-lambdas in the
  // original code are now Context methods; keep local aliases
  // for source-compatibility with the moved block.
  auto registerAggr = [&](const rsm2pdb::rsm::AggregateType *a) {
    return ctx.registerAggr(a);
  };
  auto lookupAggrIdx = [&](const rsm2pdb::rsm::Variable &v) {
    return ctx.lookupAggrIdx(v);
  };

  if (have_rsm) {
    agg_by_name.reserve(rsm_reader.aggregates().size());
    agg_by_unit_name.reserve(rsm_reader.aggregates().size());
    for (const auto &a : rsm_reader.aggregates()) {
      agg_by_name.emplace(a.name, &a);
      agg_by_unit_name.emplace(
          pdb_detail::UA_Name{a.unit_anchor_offset, a.name}, &a);
    }
  }

  if (have_rsm) {
    std::size_t typed_globals = 0;
    std::size_t aggr_globals = 0;
    for (auto &ps : inputs.publics) {
      if (ps.is_function)
        continue;
      if (ps.segment == 0 || ps.segment > inputs.sections.size())
        continue;
      const auto &pe_sec = inputs.sections[ps.segment - 1];
      const std::uint64_t va = image_base + pe_sec.virtual_address + ps.offset;
      const auto *v = rsm_reader.findVariableAt(va);
      if (!v)
        continue;
      // Primitive globals: resolve via Pascal-name -> primitive.
      if (!v->pascal_type.empty()) {
        if (auto rp = rsm2pdb::rsm::Reader::resolvePrimitive(v->pascal_type)) {
          ps.byte_size = rp->byte_size;
          ps.prim_kind = rp->kind;
          ++typed_globals;
          continue;
        }
      }
      // Non-primitive globals: route through aggregate registrar.
      // Records get LF_STRUCTURE in the PDB; classes / enums
      // stay on the byte[N] fallback for now.
      if (auto idx = lookupAggrIdx(*v)) {
        ps.aggregate_index = idx;
        ps.byte_size = inputs.aggregates[*idx].byte_size != 0
                           ? inputs.aggregates[*idx].byte_size
                           : ps.byte_size;
        ++aggr_globals;
      }
    }
    std::fprintf(stdout,
                 "globals typed via RSM pascal_type: %zu / %zu "
                 "(records routed via TPI: %zu, %zu aggregates "
                 "registered)\n",
                 typed_globals, inputs.publics.size(), aggr_globals,
                 inputs.aggregates.size());
  }
}

} // namespace rsm2pdb::cli::pdb_detail
