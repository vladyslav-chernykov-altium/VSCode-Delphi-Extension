// cli_cmd_pdb_thunks.cpp -- adjuster-thunk detection + emit.
// Part of the cli_cmd_pdb split; see cli_cmd_pdb_internal.h.

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

// (isRtlQName lives in cli_cmd_pdb_aggregates.cpp -- it has no
// runtime state and reads more naturally next to the RTL filter
// consumers, but is logically a shared helper.)

void detectAdjusterThunks(Context &ctx) {
  auto &inputs = ctx.inputs;
  const auto &mf = ctx.map_reader.file();
  auto &thunks_to_emit = ctx.thunks_to_emit;
  const auto &pe_bytes = ctx.pe_bytes;
  const auto image_base = ctx.image_base;
  auto &seg_is_code = ctx.seg_is_code;
  auto stamp = [&ctx](const char *tag) { ctx.stamp(tag); };

  // -- Interface adjuster-thunk detection -----------------------
  //
  // Pattern: `48 83 c1 NN  e9 NN NN NN NN` (9 bytes), Self-adjust
  // + tail-call into the real method. Without an explicit S_PROC32
  // + a C13 line entry covering this PC, cppvsdbg silently demotes
  // step-into-the-thunk to step-over and the user never sees the
  // method body. We resolve each thunk's jmp target against the
  // .map publics; matched thunks get synthesised
  // S_PUB32 + S_GPROC32 entries (named
  // `<TargetMethod>$Adjust_<HexImm>`) and a single line entry so
  // cppvsdbg recognises them as user code. Additionally we
  // generate a sibling `.natstepfilter` listing
  // `.*\$Adjust_[0-9A-F]+$` -- VS2022 / cppvsdbg honour it and
  // tail-call-skip through the thunks on Step Into, landing the
  // user directly in the real method body.
  {
    // Build the code-section descriptors the scanner needs.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> code_secs;
    std::vector<std::pair<std::uint32_t, std::uint16_t>> sec_file;
    for (std::uint16_t i = 0; i < inputs.sections.size(); ++i) {
      const auto &s = inputs.sections[i];
      if ((s.characteristics & 0x20u) == 0)
        continue; // !CNT_CODE
      code_secs.emplace_back(static_cast<std::uint32_t>(s.virtual_address),
                             std::max(s.virtual_size, s.size_of_raw_data));
      sec_file.emplace_back(static_cast<std::uint32_t>(s.pointer_to_raw_data),
                            static_cast<std::uint16_t>(i + 1));
    }
    const auto detected = rsm2pdb::pe::scanAdjusterThunks(pe_bytes, code_secs,
                                                          sec_file, image_base);

    // Index the original .map publics by VA for jmp-target
    // resolution. Use the unfiltered list -- thunks to RTL
    // methods (e.g. System.TInterfacedObject._AddRef) are real
    // thunks and we want to mask them out too via the same
    // .natstepfilter pattern.
    std::unordered_map<std::uint64_t, const rsm2pdb::map::Public *> va_to_pub;
    va_to_pub.reserve(mf.publics.size());
    for (const auto &p : mf.publics) {
      const auto va =
          (mf.findSegment(p.segment_id) ? mf.findSegment(p.segment_id)->start_va
                                        : image_base) +
          p.segment_offset;
      va_to_pub.emplace(va, &p);
    }

    // .map module_segment + line-table lookup for "which unit
    // does this target VA belong to and what's its first source
    // line" -- the thunk gets attached to that unit's PDB
    // module and points its C13 line entry at the target's
    // first source line.
    //
    // Both helpers used to be linear scans inside the per-thunk
    // loop. On real-world projects (AdvPCB: 947k publics,
    // 142k line-tables, ~5M line records, ~tens-of-thousands of
    // thunks) the inner double-loop in findLineForVa was the
    // hot path -- ~10^11 iterations, several minutes wall-time.
    // Pre-build sorted indexes once and binary-search per
    // thunk: O(log N) per lookup instead of O(N).

    // Unit-by-VA: vector of (va_start, va_end, &unit_name).
    struct UnitSpan {
      std::uint64_t va_start;
      std::uint64_t va_end;
      const std::string *unit;
    };
    std::vector<UnitSpan> unit_spans;
    unit_spans.reserve(mf.module_segments.size());
    for (const auto &ms : mf.module_segments) {
      if (ms.segment_id != 1)
        continue;
      const auto *seg = mf.findSegment(ms.segment_id);
      if (!seg)
        continue;
      UnitSpan s;
      s.va_start = seg->start_va + ms.segment_offset;
      s.va_end = s.va_start + ms.length;
      s.unit = &ms.module_name;
      unit_spans.push_back(s);
    }
    std::sort(unit_spans.begin(), unit_spans.end(),
              [](const UnitSpan &a, const UnitSpan &b) {
                return a.va_start < b.va_start;
              });
    auto findUnitForVa = [&](std::uint64_t va) -> std::string {
      auto it = std::upper_bound(
          unit_spans.begin(), unit_spans.end(), va,
          [](std::uint64_t v, const UnitSpan &s) { return v < s.va_start; });
      if (it == unit_spans.begin())
        return {};
      --it;
      return (va < it->va_end) ? *it->unit : std::string{};
    };

    // Line-by-VA: flat vector of (va, line) sorted by va. Each
    // entry collapses any duplicates (same VA, multiple lines)
    // to the smallest line number -- that's the natural "first
    // source line at this PC" semantic.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> line_by_va;
    {
      std::size_t total = 0;
      for (const auto &lt : mf.line_tables)
        total += lt.lines.size();
      line_by_va.reserve(total);
      for (const auto &lt : mf.line_tables) {
        for (const auto &lr : lt.lines) {
          const auto *seg = mf.findSegment(lr.segment_id);
          if (!seg)
            continue;
          const auto lva = seg->start_va + lr.segment_offset;
          line_by_va.emplace_back(lva, lr.line);
        }
      }
      std::sort(line_by_va.begin(), line_by_va.end(),
                [](const auto &a, const auto &b) { return a.first < b.first; });
    }
    auto findLineForVa = [&](std::uint64_t va) -> std::uint32_t {
      if (line_by_va.empty())
        return 1;
      auto it = std::upper_bound(
          line_by_va.begin(), line_by_va.end(), va,
          [](std::uint64_t v, const auto &p) { return v < p.first; });
      if (it == line_by_va.begin()) {
        return it->first == va ? it->second : 1;
      }
      --it;
      return it->second;
    };

    std::size_t total_thunks = static_cast<std::size_t>(detected.size());
    std::size_t resolved = 0;
    for (const auto &t : detected) {
      auto it = va_to_pub.find(t.target_va);
      if (it == va_to_pub.end())
        continue; // unresolved jmp
      const auto &tgt = *it->second;
      if (!seg_is_code[tgt.segment_id])
        continue;

      // Section coords of the thunk itself.
      const auto &pe_sec_t = inputs.sections[t.segment - 1];
      // sanity guard
      if (t.section_off + rsm2pdb::pe::AdjusterThunk::kSize >
          pe_sec_t.size_of_raw_data)
        continue;

      ThunkEmit te;
      te.va = t.va;
      te.target_va = t.target_va;
      te.adjustment = t.adjustment;
      te.segment = t.segment;
      te.offset = t.section_off;
      // Synthesise the name: <target>$Adjust_<HexImm>. HexImm
      // is the 32-bit two's-complement of the adjustment so
      // the value is always positive in the regex
      // .*\$Adjust_[0-9A-F]+$.
      const auto adj_u32 = static_cast<std::uint32_t>(t.adjustment);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "$Adjust_%08X", adj_u32);
      te.name = tgt.name + buf;
      te.target_unit = findUnitForVa(t.target_va);
      te.target_line = findLineForVa(t.target_va);
      thunks_to_emit.push_back(std::move(te));
      ++resolved;
    }
    std::fprintf(stdout,
                 "adjuster thunks: %zu found, %zu resolved to known methods\n",
                 total_thunks, resolved);
  }

  // Emit S_PUB32 entries for resolved thunks now so they show up
  // in Call Stack with a meaningful name. Their per-module
  // S_GPROC32 + line entries are added later inside the module
  // composer (we group them by target unit).
  for (const auto &te : thunks_to_emit) {
    rsm2pdb::pdb::PublicSymbol ps;
    ps.name = te.name;
    ps.segment = te.segment;
    ps.offset = te.offset;
    ps.is_function = true;
    inputs.publics.push_back(std::move(ps));
  }
}

} // namespace rsm2pdb::cli::pdb_detail
