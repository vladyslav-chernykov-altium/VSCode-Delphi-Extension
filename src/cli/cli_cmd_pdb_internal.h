#pragma once

// Internal context + phase declarations shared across the cli_cmd_pdb_*
// submodules. Public clients invoke cli::cmdPdb() only (declared in
// cli.h); this header is .cpp-private to the cli_cmd_pdb family.

#include "map/map_reader.h"
#include "pdb/pdb_writer.h"
#include "rsm/rsm_reader.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rsm2pdb::cli::pdb_detail {

// Synthesised adjuster-thunk PDB entry. Detected by
// pe::scanAdjusterThunks; emitted as S_PUB32 and (later, per-module)
// S_GPROC32 + line entry pointing at the target method's first line.
struct ThunkEmit {
  std::uint64_t va;
  std::uint64_t target_va;
  std::int32_t adjustment;
  std::uint16_t segment;
  std::uint32_t offset;      // PE-section-relative
  std::string name;          // synthesised PDB symbol name
  std::string target_unit;   // .map unit that owns the target
  std::uint32_t target_line; // first source line of the target
};

// Composite key (unit_anchor_offset, name) used by the per-unit
// aggregate-by-name multimap. Same-unit lookup wins over cross-unit
// when both kinds match.
struct UA_Name {
  std::uint64_t ua;
  std::string name;
  bool operator==(const UA_Name &o) const {
    return ua == o.ua && name == o.name;
  }
};
struct UA_Name_Hash {
  std::size_t operator()(const UA_Name &k) const noexcept {
    return std::hash<std::uint64_t>{}(k.ua) * 1099511628211ull ^
           std::hash<std::string>{}(k.name);
  }
};

// Per-invocation context carrying every cross-phase piece of state
// the cli pdb pipeline needs. Owned by cmdPdb(); each phase function
// takes Context& and reads/writes the relevant fields.
struct Context {
  // ---- Args (set during arg parsing) ----
  std::string map_path;
  std::string input_exe;
  std::string output_exe; // == input_exe (in-place)
  std::string output_pdb;
  std::vector<std::string> src_search_dirs;
  bool include_rtl = false;
  // NatVis sidecar emission: ON by default; turn off with the
  // --no-natvis CLI flag. The companion `.natvis` lives next to
  // the PDB and is auto-loaded by cppvsdbg (VS native + VSCode).
  bool emit_natvis = true;

  // ---- PE bytes + parsed header pointers ----
  std::vector<std::uint8_t> pe_bytes;
  std::uint64_t image_base = 0;

  // ---- .map reader ----
  rsm2pdb::map::Reader map_reader;
  // Segment-id -> CODE/DATA classifier built from .map.
  std::unordered_map<std::uint16_t, bool> seg_is_code;

  // ---- .rsm reader (optional) ----
  rsm2pdb::rsm::Reader rsm_reader;
  bool have_rsm = false;

  // ---- PdbInputs being built across phases ----
  rsm2pdb::pdb::PdbInputs inputs;

  // ---- Thunks (produced by detectAdjusterThunks) ----
  std::vector<ThunkEmit> thunks_to_emit;

  // ---- Aggregate registrar state ----
  std::map<std::pair<std::uint64_t, std::uint16_t>, std::size_t> aggr_idx_cache;
  // Cycle-break guard for registerAggr's recursion through bases /
  // composite-typed fields. Prevents STATUS_STACK_OVERFLOW on
  // AdvPCB-scale projects (deep VCL hierarchies + generic container
  // owner-loops).
  std::set<std::pair<std::uint64_t, std::uint16_t>> aggr_in_progress;
  // Self placeholders for opaque / forward-declared classes whose
  // 0x2a record carries no fields (AdvPCB's TPCBCommands etc.).
  // Keyed by class name -- one synthetic LF_CLASS per class, not
  // per method.
  std::map<std::string, std::size_t> self_opaque_cache;
  // Anonymous in-proc set placeholders. Keyed by (unit_anchor,
  // base_enum_own_hash) so multiple `r: set of TXxx` locals
  // sharing the same enum get ONE LF_BITFIELD struct.
  std::map<std::pair<std::uint64_t, std::uint16_t>, std::size_t>
      inproc_set_cache;
  // Pre-built indexes over rsm_reader.aggregates() so per-Self
  // class-name lookups run O(1) instead of O(N).
  std::unordered_multimap<std::string, const rsm2pdb::rsm::AggregateType *>
      agg_by_name;
  std::unordered_multimap<UA_Name, const rsm2pdb::rsm::AggregateType *,
                          UA_Name_Hash>
      agg_by_unit_name;

  // ---- Per-marker byte-size aggregate (used by compose::
  // resolveFunction inside the module composer). ----
  std::unordered_map<std::uint8_t, std::uint32_t> marker_sizes;

  // ---- Source-path resolution cache ----
  std::unordered_map<std::string, std::string> src_cache;

  // ---- Per-phase timing ----
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point prev = std::chrono::steady_clock::now();
  void stamp(const char *tag);

  // ---- Helpers (defined in cli_cmd_pdb.cpp) ----
  std::uint64_t rvaOfPublic(const rsm2pdb::map::Public &p) const;
  std::uint16_t findPeSection(std::uint64_t rva) const;
  std::string resolveSourceCached(const std::string &raw);

  // ---- Aggregate registrar (defined in cli_cmd_pdb_aggregates.cpp).
  std::optional<std::size_t> registerAggr(const rsm2pdb::rsm::AggregateType *a);
  std::optional<std::size_t> lookupAggrIdx(const rsm2pdb::rsm::Variable &v);
};

// True when the given Pascal-qualified name (unit or dotted-member)
// belongs to a Delphi-shipped framework unit we have no proper debug
// info for. Used to strip RTL publics + modules by default. See
// cli_cmd_pdb.cpp for the prefix list.
bool isRtlQName(std::string_view q);

// ---- Phase entry points (each takes Context& and mutates it). ----

// Phase: scan code sections for adjuster thunks, resolve targets to
// .map publics, emit S_PUB32 entries. Populates ctx.thunks_to_emit
// and pushes adjuster names into ctx.inputs.publics.
void detectAdjusterThunks(Context &ctx);

// Phase: enrich already-emitted globals with their RSM-resolved
// Pascal primitive / aggregate type so S_GDATA32 records carry real
// CodeView types instead of generic void*. Also builds the
// aggregate-registry indexes.
void enrichGlobalsViaRsm(Context &ctx);

// Phase: compose one DBI module per Pascal compile unit. Emits
// S_GPROC32 + line subsections + per-function locals (S_REGREL32)
// via compose::resolveFunction.
void composeModules(Context &ctx);

} // namespace rsm2pdb::cli::pdb_detail
