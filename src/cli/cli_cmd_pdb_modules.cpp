// cli_cmd_pdb_modules.cpp -- per-CU DBI module composition
// (S_GPROC32 + line subsections + per-function locals via
// compose::resolveFunction). Part of the cli_cmd_pdb split.

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

void composeModules(Context &ctx) {
  auto &inputs = ctx.inputs;
  auto &rsm_reader = ctx.rsm_reader;
  const bool have_rsm = ctx.have_rsm;
  const auto &mf = ctx.map_reader.file();
  const auto image_base = ctx.image_base;
  auto &thunks_to_emit = ctx.thunks_to_emit;
  const auto &marker_sizes = ctx.marker_sizes;
  const auto &map_path = ctx.map_path;
  const auto &src_search_dirs = ctx.src_search_dirs;
  const auto &pe_bytes = ctx.pe_bytes;
  const bool include_rtl = ctx.include_rtl;
  auto stamp = [&ctx](const char *tag) { ctx.stamp(tag); };
  auto findPeSection = [&ctx](std::uint64_t rva) {
    return ctx.findPeSection(rva);
  };
  auto resolveSourceCached = [&ctx](const std::string &raw) {
    return ctx.resolveSourceCached(raw);
  };
  auto registerAggr = [&ctx](const rsm2pdb::rsm::AggregateType *a) {
    return ctx.registerAggr(a);
  };
  auto lookupAggrIdx = [&ctx](const rsm2pdb::rsm::Variable &v) {
    return ctx.lookupAggrIdx(v);
  };
  auto &inproc_set_cache = ctx.inproc_set_cache;
  auto &self_opaque_cache = ctx.self_opaque_cache;
  auto &aggr_idx_cache = ctx.aggr_idx_cache;
  auto &agg_by_name = ctx.agg_by_name;
  auto &agg_by_unit_name = ctx.agg_by_unit_name;

  stamp("phase: compose modules");
  // 2b. Compose modules from .map: one DBI module per Pascal
  //     compile unit (= one line_table) carrying its source
  //     path, functions, and line entries. Functions come from
  //     publics whose VA falls in a module_segment for the unit.
  {
    // Index module_segments in segment 1 (.text) by VA range.
    struct UnitRange {
      std::string unit;
      std::uint64_t va_start;
      std::uint64_t va_end;
    };
    std::vector<UnitRange> code_ranges;
    for (const auto &ms : mf.module_segments) {
      if (ms.segment_id != 1)
        continue;
      const auto *seg = mf.findSegment(ms.segment_id);
      if (!seg)
        continue;
      UnitRange r;
      r.unit = ms.module_name;
      r.va_start = seg->start_va + ms.segment_offset;
      r.va_end = r.va_start + ms.length;
      code_ranges.push_back(std::move(r));
    }
    std::sort(code_ranges.begin(), code_ranges.end(),
              [](const UnitRange &a, const UnitRange &b) {
                return a.va_start < b.va_start;
              });
    auto findUnit = [&](std::uint64_t va) -> const UnitRange * {
      auto it = std::upper_bound(
          code_ranges.begin(), code_ranges.end(), va,
          [](std::uint64_t v, const UnitRange &r) { return v < r.va_start; });
      if (it == code_ranges.begin())
        return nullptr;
      --it;
      if (va < it->va_end)
        return &*it;
      return nullptr;
    };

    // Collect functions per unit (segment 1 publics only).
    struct FnRaw {
      std::string name;
      std::uint64_t va;
    };
    std::map<std::string, std::vector<FnRaw>> by_unit;
    for (const auto &p : mf.publics) {
      if (p.segment_id != 1)
        continue;
      const std::uint64_t va =
          mf.findSegment(p.segment_id)->start_va + p.segment_offset;
      const auto *ur = findUnit(va);
      if (!ur)
        continue;
      by_unit[ur->unit].push_back({p.name, va});
    }

    // Build the module list. A single Pascal unit can have
    // multiple line_tables (one per source file pulled in via
    // {$INCLUDE} or named alongside the unit) -- aggregate
    // them by unit name so all line entries make it into the
    // module's C13 subsection.
    std::unordered_map<std::string,
                       std::vector<const rsm2pdb::map::LineTable *>>
        by_name;
    for (const auto &lt : mf.line_tables) {
      by_name[lt.module_name].push_back(&lt);
    }
    std::set<std::string> unit_names;
    for (const auto &kv : by_unit)
      unit_names.insert(kv.first);
    for (const auto &lt : mf.line_tables)
      unit_names.insert(lt.module_name);

    std::size_t total_fns = 0, total_lines = 0;
    std::size_t skipped_modules = 0;
    for (const auto &uname : unit_names) {
      if (!include_rtl && isRtlQName(uname)) {
        ++skipped_modules;
        continue;
      }
      rsm2pdb::pdb::Module pdb_mod;
      pdb_mod.name = uname;
      auto it_lts = by_name.find(uname);
      const std::vector<const rsm2pdb::map::LineTable *> *lts =
          it_lts != by_name.end() ? &it_lts->second : nullptr;

      // Sort functions in this unit by VA so we can compute
      // sizes as next-VA gaps.
      auto &raws = by_unit[uname];
      std::sort(raws.begin(), raws.end(),
                [](const FnRaw &a, const FnRaw &b) { return a.va < b.va; });
      for (std::size_t i = 0; i < raws.size(); ++i) {
        const auto &r = raws[i];
        const std::uint64_t rva = r.va - image_base;
        const std::uint16_t seg_idx = findPeSection(rva);
        if (seg_idx == 0)
          continue;
        const auto &pe_sec = inputs.sections[seg_idx - 1];
        std::uint64_t end_va;
        if (i + 1 < raws.size()) {
          end_va = raws[i + 1].va;
        } else {
          const auto *ur = findUnit(r.va);
          end_va = ur ? ur->va_end : (r.va + 1);
        }
        rsm2pdb::pdb::ModuleFunction mf_out;
        mf_out.name = r.name;
        mf_out.segment = seg_idx;
        mf_out.offset =
            static_cast<std::uint32_t>(rva - pe_sec.virtual_address);
        mf_out.size = static_cast<std::uint32_t>(end_va - r.va);

        // Params + locals via compose::resolveFunction --
        // the single source-of-truth Delphi-x64 frame
        // interpretation (sub_rsp / extra_pushes / Self
        // marker / 2-byte-form signed decode / size-
        // resolution chain).
        if (have_rsm) {
          if (const auto *pr = rsm_reader.findProcedureAt(r.va)) {
            // Slice of PE bytes the prologue / disasm-
            // sniffer will read. Empty when the proc's
            // bytes fall outside the PE; resolveFunction
            // degrades to sub_rsp=0 in that case.
            const auto &pe_sec_fn = inputs.sections[mf_out.segment - 1];
            const std::size_t fn_fo =
                pe_sec_fn.pointer_to_raw_data + mf_out.offset;
            const std::uint8_t *code = nullptr;
            std::size_t code_len = 0;
            if (fn_fo + mf_out.size <= pe_bytes.size()) {
              code = pe_bytes.data() + fn_fo;
              code_len = mf_out.size;
            }
            const auto rf = rsm2pdb::compose::resolveFunction(
                *pr, code, code_len, marker_sizes);
            for (const auto &rv : rf.vars) {
              rsm2pdb::pdb::ModuleLocal ml;
              ml.name = rv.name;
              ml.is_param = rv.is_param;
              ml.offset = rv.rbp_offset;
              ml.byte_size = rv.byte_size;
              ml.prim_kind = rv.prim_kind;
              // Phase D: route record-typed locals
              // through the aggregate registrar so they
              // emit LF_STRUCTURE-backed TypeIndex
              // instead of byte[N]. compose stamps
              // aggregate_hash + unit_anchor_offset on
              // the resolved var; we look the aggregate
              // up and register it (no-op if already
              // seen).
              if (rv.aggregate_hash != 0) {
                const rsm2pdb::rsm::AggregateType *a =
                    rsm_reader.findAggregateInUnit(rv.unit_anchor_offset,
                                                   rv.aggregate_hash);
                if (a == nullptr) {
                  a = rsm_reader.findAggregateByHash(rv.aggregate_hash);
                }
                if (auto idx = registerAggr(a)) {
                  ml.aggregate_index = idx;
                  if (inputs.aggregates[*idx].byte_size)
                    ml.byte_size = inputs.aggregates[*idx].byte_size;
                }
              }
              // Anonymous in-proc `set of TXxx` local.
              // Delphi declares the set's type
              // implicitly (no 0x2a record), so r's
              // type_hash won't resolve. The companion
              // 0x2a record for the BASE ENUM sits a
              // few hashes earlier in the same unit;
              // empirically the set's hash =
              // enum_hash + 4. If we find one within
              // a small window, synthesise an
              // LF_BITFIELD-struct view using the
              // enum's enumerators -- cdb / VS show
              // which bits are set instead of an
              // opaque void*.
              if (rv.aggregate_hash != 0 && !ml.aggregate_index &&
                  !rv.is_self) {
                const rsm2pdb::rsm::AggregateType *enum_a = nullptr;
                int best_diff = 16;
                for (const auto &a : rsm_reader.aggregates()) {
                  if (a.kind != rsm2pdb::rsm ::AggregateKind::Enum)
                    continue;
                  if (a.unit_anchor_offset != rv.unit_anchor_offset)
                    continue;
                  const int diff = static_cast<int>(rv.aggregate_hash) -
                                   static_cast<int>(a.own_hash);
                  if (diff >= 1 && diff <= best_diff) {
                    best_diff = diff;
                    enum_a = &a;
                  }
                }
                if (enum_a != nullptr) {
                  const auto key =
                      std::make_pair(rv.unit_anchor_offset, enum_a->own_hash);
                  std::size_t set_idx;
                  auto it = inproc_set_cache.find(key);
                  if (it != inproc_set_cache.end()) {
                    set_idx = it->second;
                  } else {
                    rsm2pdb::pdb::AggregateRecord rec;
                    rec.kind = rsm2pdb::pdb ::AggregateKind::Set;
                    rec.name = "set of " + enum_a->name;
                    std::int64_t max_ord = 0;
                    rec.enumerators.reserve(enum_a->enum_entries.size());
                    for (const auto &e : enum_a->enum_entries) {
                      rsm2pdb::pdb ::AggregateEnumerator ae;
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
                    set_idx = inputs.aggregates.size();
                    inputs.aggregates.push_back(std::move(rec));
                    inproc_set_cache[key] = set_idx;
                  }
                  ml.aggregate_index = set_idx;
                  ml.byte_size = inputs.aggregates[set_idx].byte_size;
                }
              }
              // Self of a class method: ALWAYS derive
              // type from the enclosing class via the
              // proc-name suffix instead of trusting
              // RSM's Self encoding (which can carry
              // sentinel markers like 0x29 / 0xD5 that
              // don't resolve to an aggregate, or a
              // 2-byte encoded marker like 0x0405 that
              // matches nothing).
              //
              // Pascal `procedure TClass.Method` -> the
              // implicit Self has type `TClass*`. Strip
              // the trailing `.Method`, then strip the
              // leading unit prefix. Look the class up:
              //   1. Same unit, kind = Class    (best)
              //   2. Any unit,  kind = Class    (class
              //      declared in interface-only unit)
              //   3. Any unit,  any kind        (opaque
              //      / forward-declared classes, AdvPCB
              //      style -- TPCBCommands has a 0x2a
              //      record but no class header or
              //      fields). registerAsClass synthesises
              //      an empty LF_CLASS so cdb at least
              //      knows it's a typed pointer.
              //   4. Nothing found              -> emit
              //      Self as void* (typeForSize(0)
              //      yields VoidPointer64, the only
              //      pointer-ish primitive we have).
              if (rv.is_self) {
                const auto &pn = pr->name;
                const auto last_dot = pn.rfind('.');
                std::string class_name;
                if (last_dot != std::string::npos && last_dot > 0) {
                  const auto prev_dot = pn.rfind('.', last_dot - 1);
                  class_name =
                      (prev_dot == std::string::npos)
                          ? pn.substr(0, last_dot)
                          : pn.substr(prev_dot + 1, last_dot - prev_dot - 1);
                }
                if (!class_name.empty()) {
                  const auto ua = rsm_reader.unitAnchorFor(pr->file_offset);
                  const rsm2pdb::rsm::AggregateType *cls = nullptr;
                  // Pass 1: same unit, kind=Class.
                  auto r1 =
                      agg_by_unit_name.equal_range(UA_Name{ua, class_name});
                  for (auto it = r1.first; it != r1.second && !cls; ++it) {
                    if (it->second->kind ==
                        rsm2pdb::rsm ::AggregateKind::Class) {
                      cls = it->second;
                    }
                  }
                  // Pass 2: any unit, kind=Class.
                  if (cls == nullptr) {
                    auto r2 = agg_by_name.equal_range(class_name);
                    for (auto it = r2.first; it != r2.second && !cls; ++it) {
                      if (it->second->kind ==
                          rsm2pdb::rsm ::AggregateKind ::Class) {
                        cls = it->second;
                      }
                    }
                  }
                  // Pass 3: any kind, any unit
                  // (TPCBCommands-style opaque).
                  if (cls == nullptr) {
                    auto r3 = agg_by_name.equal_range(class_name);
                    if (r3.first != r3.second) {
                      cls = r3.first->second;
                    }
                  }
                  std::optional<std::size_t> self_idx;
                  if (cls != nullptr &&
                      cls->kind == rsm2pdb::rsm ::AggregateKind::Class) {
                    // Full class -- normal path.
                    self_idx = registerAggr(cls);
                  } else {
                    // Opaque / not found:
                    // synthesise an empty
                    // LF_CLASS placeholder so
                    // Self at least shows up as
                    // `TClassName *` with no
                    // fields. Cached by name.
                    auto cit = self_opaque_cache.find(class_name);
                    if (cit != self_opaque_cache.end()) {
                      self_idx = cit->second;
                    } else {
                      const auto idx = inputs.aggregates.size();
                      rsm2pdb::pdb::AggregateRecord rec;
                      rec.kind = rsm2pdb::pdb ::AggregateKind::Class;
                      rec.name = class_name;
                      rec.byte_size = 0;
                      inputs.aggregates.push_back(std::move(rec));
                      self_opaque_cache[class_name] = idx;
                      self_idx = idx;
                    }
                  }
                  if (self_idx) {
                    ml.aggregate_index = self_idx;
                    ml.byte_size = 8;
                  } else {
                    // No class info AT ALL --
                    // void* hint is still better
                    // than unsigned __int64.
                    ml.byte_size = 0;
                    ml.prim_kind = std::nullopt;
                  }
                }
              }
              mf_out.locals.push_back(std::move(ml));
            }

            // FE.1.5 + FE.2: register class method into its owning
            // aggregate so emitAggregates can wire LF_MFUNCTION +
            // LF_ONEMETHOD. Naming convention:
            // `<unit>.<ClassName>.<MethodName>`. Lives inside the
            // rf-scope (rather than a separate `if (have_rsm)`
            // block) so we can harvest resolved param types from
            // `rf.vars` for the LF_ARGLIST builder.
            //
            // FE.1.5 (lazy-register): if the class isn't in
            // inputs.aggregates yet, register it from RSM on demand
            // -- methods are emitted in source-declaration order
            // BEFORE the user function whose local would trigger
            // registration (e.g. 08_inherit_props's TDog).
            //
            // FE.2 (params): drop the FE.1 `pr->params.size() <= 1`
            // gate. Collect non-Self params from rf.vars in order;
            // the writer turns them into an LF_ARGLIST and links
            // it from this method's LF_MFUNCTION. Also detect
            // `procedure` vs `function` from whether the proc has
            // a local named "Result" -- procedures get T_VOID
            // return type, functions get the Int32 default.
            const auto& name = mf_out.name;
            const auto last_dot = name.find_last_of('.');
            if (last_dot != std::string::npos && last_dot > 0) {
              const auto second_dot =
                  name.find_last_of('.', last_dot - 1);
              if (second_dot != std::string::npos) {
                const std::string class_name =
                    name.substr(second_dot + 1,
                                last_dot - second_dot - 1);
                const std::string method_name =
                    name.substr(last_dot + 1);

                rsm2pdb::pdb::AggregateMethod am;
                am.name = method_name;
                am.qualified_name = mf_out.name;
                for (const auto &rv : rf.vars) {
                  if (!rv.is_param || rv.is_self || rv.is_static_link)
                    continue;
                  // Hidden Pascal-only params like `.` (the unnamed
                  // byref-return-string slot) -- skip; cppvsdbg can't
                  // call them with a meaningful value anyway.
                  if (rv.name.empty() || rv.name == ".")
                    continue;
                  rsm2pdb::pdb::AggregateMethod::Param p;
                  p.prim_kind = rv.prim_kind;
                  p.byte_size = rv.byte_size;
                  if (rv.aggregate_hash != 0) {
                    const rsm2pdb::rsm::AggregateType *a =
                        rsm_reader.findAggregateInUnit(
                            rv.unit_anchor_offset, rv.aggregate_hash);
                    if (a == nullptr)
                      a = rsm_reader.findAggregateByHash(rv.aggregate_hash);
                    if (auto idx = registerAggr(a))
                      p.aggregate_index = idx;
                  }
                  am.params.push_back(std::move(p));
                }
                bool has_result_local = false;
                for (const auto &l : pr->locals) {
                  if (l.name == "Result") {
                    has_result_local = true;
                    break;
                  }
                }
                am.returns_void = !has_result_local;

                bool registered = false;
                for (auto& a : inputs.aggregates) {
                  if (a.kind != rsm2pdb::pdb::AggregateKind::Class)
                    continue;
                  if (a.name != class_name) continue;
                  a.methods.push_back(std::move(am));
                  registered = true;
                  break;
                }
                if (!registered) {
                  // Lazy-register the class from RSM.
                  const rsm2pdb::rsm::AggregateType *foreign = nullptr;
                  auto r2 = agg_by_name.equal_range(class_name);
                  for (auto it = r2.first; it != r2.second; ++it) {
                    if (it->second->kind
                        == rsm2pdb::rsm::AggregateKind::Class) {
                      foreign = it->second;
                      break;
                    }
                  }
                  if (foreign != nullptr) {
                    if (auto idx = registerAggr(foreign)) {
                      inputs.aggregates[*idx].methods.push_back(
                          std::move(am));
                    }
                  }
                }
              }
            }
          }
        }

        pdb_mod.functions.push_back(std::move(mf_out));
      }

      // Inject the adjuster thunks belonging to this unit.
      // Each gets a tiny S_GPROC32 entry (size = 9 bytes,
      // matching the `add rcx, imm8; jmp rel32` shape) so
      // cppvsdbg recognises the PC as user code and steps
      // into it; without this the engine demotes Step Into
      // to Step Over on the indirect call that targets the
      // thunk. The single line entry below points at the
      // target's first source line -- the user briefly sees
      // that source location during the descent through the
      // thunk, then lands in the real method.
      for (const auto &te : thunks_to_emit) {
        if (te.target_unit != uname)
          continue;
        rsm2pdb::pdb::ModuleFunction tf;
        tf.name = te.name;
        tf.segment = te.segment;
        tf.offset = te.offset;
        tf.size = rsm2pdb::pe::AdjusterThunk::kSize;
        pdb_mod.functions.push_back(std::move(tf));
      }
      total_fns += pdb_mod.functions.size();

      // Line entries: translate .map LineRecord coords -> PE
      // section-relative (segment, offset). Each .map
      // LineTable becomes one ModuleSource so multi-file
      // units keep all their lines.
      if (lts) {
        for (const auto *lt : *lts) {
          rsm2pdb::pdb::ModuleSource src;
          src.source_path = resolveSourceCached(lt->source_path);
          for (const auto &lr : lt->lines) {
            const auto *seg = mf.findSegment(lr.segment_id);
            if (!seg)
              continue;
            const std::uint64_t va = seg->start_va + lr.segment_offset;
            const std::uint64_t rva = va - image_base;
            const std::uint16_t seg_idx = findPeSection(rva);
            if (seg_idx == 0)
              continue;
            const auto &pe_sec = inputs.sections[seg_idx - 1];
            rsm2pdb::pdb::ModuleLine ml;
            ml.segment = seg_idx;
            ml.offset =
                static_cast<std::uint32_t>(rva - pe_sec.virtual_address);
            ml.line = lr.line;
            src.lines.push_back(ml);
          }
          // Append thunk line entries to the FIRST source
          // of this unit (whatever the .map's primary
          // line-table is). The single per-thunk line
          // entry covers the thunk's PC range (9 bytes)
          // and points at the target method's first
          // source line -- enough for cppvsdbg to treat
          // the thunk as user code instead of degrading
          // Step Into to Step Over.
          if (lt == (*lts)[0]) {
            for (const auto &te : thunks_to_emit) {
              if (te.target_unit != uname)
                continue;
              rsm2pdb::pdb::ModuleLine ml;
              ml.segment = te.segment;
              ml.offset = te.offset;
              ml.line = te.target_line;
              src.lines.push_back(ml);
            }
          }
          total_lines += src.lines.size();
          if (!src.lines.empty()) {
            pdb_mod.sources.push_back(std::move(src));
          }
        }
      }

      inputs.modules.push_back(std::move(pdb_mod));
    }
    std::fprintf(
        stdout,
        "modules: %zu (RTL-filtered %zu), S_GPROC32: %zu, line entries: %zu\n",
        inputs.modules.size(), skipped_modules, total_fns, total_lines);
  }
}

} // namespace rsm2pdb::cli::pdb_detail
