// PDB CLI subcommand entry. Phase methods live in cli_cmd_pdb_*
// sibling files; shared state in pdb_detail::Context (declared in
// cli_cmd_pdb_internal.h).

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

namespace rsm2pdb::cli {

using pdb_detail::composeModules;
using pdb_detail::Context;
using pdb_detail::detectAdjusterThunks;
using pdb_detail::enrichGlobalsViaRsm;
using pdb_detail::isRtlQName;

// ---- Context helpers ----

namespace clk_ns = std::chrono;
using clk = clk_ns::steady_clock;

void Context::stamp(const char *tag) {
  auto now = clk::now();
  std::fprintf(stdout, "[%5.2fs] %s\n",
               clk_ns::duration<double>(now - t0).count(), tag);
  std::fflush(stdout);
  prev = now;
}

std::uint64_t Context::rvaOfPublic(const rsm2pdb::map::Public &p) const {
  const auto &mf = map_reader.file();
  const auto *seg = mf.findSegment(p.segment_id);
  if (!seg)
    return 0;
  const std::uint64_t va = seg->start_va + p.segment_offset;
  return va >= image_base ? va - image_base : va;
}

std::uint16_t Context::findPeSection(std::uint64_t rva) const {
  for (std::uint16_t i = 0; i < inputs.sections.size(); ++i) {
    const auto &s = inputs.sections[i];
    if (rva >= s.virtual_address &&
        rva < static_cast<std::uint64_t>(s.virtual_address) +
                  std::max(s.virtual_size, s.size_of_raw_data)) {
      return static_cast<std::uint16_t>(i + 1);
    }
  }
  return 0;
}

std::string Context::resolveSourceCached(const std::string &raw) {
  auto it = src_cache.find(raw);
  if (it != src_cache.end())
    return it->second;
  std::string r = resolveSourcePath(raw, map_path, src_search_dirs);
  src_cache.emplace(raw, r);
  return r;
}

int cmdPdb(int argc, char **argv) {
  Context ctx;

  // Caller has already verified argv[1] == "pdb" and argc >= 5.
  ctx.map_path = argv[2];
  ctx.input_exe = argv[3];
  ctx.output_exe = ctx.input_exe; // in-place injection
  ctx.output_pdb = argv[4];
  for (int i = 5; i < argc; ++i) {
    if (std::strcmp(argv[i], "--src-search") == 0 && i + 1 < argc) {
      ctx.src_search_dirs.emplace_back(argv[++i]);
    } else if (std::strcmp(argv[i], "--include-rtl") == 0) {
      ctx.include_rtl = true;
    } else {
      std::fprintf(stderr, "error: unknown pdb arg: %s\n", argv[i]);
      return 1;
    }
  }
  if (extLower(ctx.map_path) != "map") {
    std::fprintf(stderr, "error: first argument must be a .map file\n");
    return 1;
  }

  // ---- Load PE bytes + parse headers, build CoffSection list ----
  ctx.stamp("phase: load PE");
  {
    std::ifstream f(ctx.input_exe, std::ios::binary);
    if (!f) {
      std::fprintf(stderr, "error: cannot open %s\n", ctx.input_exe.c_str());
      return 1;
    }
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    ctx.pe_bytes.resize(static_cast<std::size_t>(sz));
    f.read(reinterpret_cast<char *>(ctx.pe_bytes.data()), sz);
  }
  const auto *dos =
      reinterpret_cast<const IMAGE_DOS_HEADER *>(ctx.pe_bytes.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    std::fprintf(stderr, "error: not a PE\n");
    return 1;
  }
  const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
      ctx.pe_bytes.data() + dos->e_lfanew);
  const auto *pe_secs = reinterpret_cast<const IMAGE_SECTION_HEADER *>(
      reinterpret_cast<const std::uint8_t *>(&nt->OptionalHeader) +
      nt->FileHeader.SizeOfOptionalHeader);
  ctx.image_base = nt->OptionalHeader.ImageBase;

  std::vector<rsm2pdb::pdb::CoffSection> coff_sections;
  coff_sections.reserve(nt->FileHeader.NumberOfSections);
  for (std::uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto &s = pe_secs[i];
    rsm2pdb::pdb::CoffSection cs;
    cs.name.assign(reinterpret_cast<const char *>(s.Name),
                   ::strnlen(reinterpret_cast<const char *>(s.Name), 8));
    cs.virtual_size = s.Misc.VirtualSize;
    cs.virtual_address = s.VirtualAddress;
    cs.size_of_raw_data = s.SizeOfRawData;
    cs.pointer_to_raw_data = s.PointerToRawData;
    cs.characteristics = s.Characteristics;
    coff_sections.push_back(std::move(cs));
  }

  // ---- Open .map ----
  ctx.stamp("phase: read .map");
  if (!ctx.map_reader.open(ctx.map_path)) {
    std::fprintf(stderr, "error: %s\n", ctx.map_reader.error().c_str());
    return 1;
  }
  const auto &mf = ctx.map_reader.file();
  std::fprintf(stdout,
               "        .map: %zu segments, %zu publics, %zu line-tables\n",
               mf.segments.size(), mf.publics.size(), mf.line_tables.size());
  std::fflush(stdout);
  for (const auto &seg : mf.segments) {
    ctx.seg_is_code[seg.id] = (seg.klass == "CODE");
  }

  ctx.inputs.sections = std::move(coff_sections);
  ctx.inputs.age = 1;
  {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    for (std::size_t i = 0; i < ctx.inputs.guid.size(); i += 8) {
      std::uint64_t r = gen();
      std::size_t take = std::min<std::size_t>(8, ctx.inputs.guid.size() - i);
      std::memcpy(ctx.inputs.guid.data() + i, &r, take);
    }
  }

  // ---- Build initial publics from .map ----
  std::size_t skipped = 0;
  std::size_t skipped_rtl = 0;
  ctx.inputs.publics.reserve(mf.publics.size());
  for (const auto &p : mf.publics) {
    if (!ctx.include_rtl && isRtlQName(p.name)) {
      ++skipped_rtl;
      continue;
    }
    const std::uint64_t rva = ctx.rvaOfPublic(p);
    const std::uint16_t seg_idx = ctx.findPeSection(rva);
    if (seg_idx == 0) {
      ++skipped;
      continue;
    }
    const auto &pe_sec = ctx.inputs.sections[seg_idx - 1];
    rsm2pdb::pdb::PublicSymbol ps;
    ps.name = p.name;
    ps.segment = seg_idx;
    ps.offset = static_cast<std::uint32_t>(rva - pe_sec.virtual_address);
    ps.is_function = ctx.seg_is_code[p.segment_id];
    ctx.inputs.publics.push_back(std::move(ps));
  }
  std::fprintf(stdout,
               "publics: %zu emitted, %zu no-PE-section, %zu RTL-filtered\n",
               ctx.inputs.publics.size(), skipped, skipped_rtl);

  // ---- Adjuster thunk detection + S_PUB32 emit ----
  detectAdjusterThunks(ctx);

  // ---- Open sibling .rsm (best-effort) ----
  {
    std::filesystem::path rp = ctx.map_path;
    rp.replace_extension(".rsm");
    if (std::filesystem::exists(rp)) {
      ctx.stamp("phase: parse .rsm");
    }
    if (std::filesystem::exists(rp) && ctx.rsm_reader.open(rp.string())) {
      ctx.have_rsm = true;
      std::fprintf(stdout, "        .rsm: %zu procs available\n",
                   ctx.rsm_reader.procedures().size());
    }
  }

  // ---- Aggregate registrar + globals enrichment ----
  enrichGlobalsViaRsm(ctx);

  // ---- Per-marker byte-size aggregate ----
  ctx.marker_sizes = ctx.have_rsm
                         ? rsm2pdb::compose::buildMarkerSizes(ctx.rsm_reader)
                         : std::unordered_map<std::uint8_t, std::uint32_t>{};

  // ---- Compose DBI modules ----
  composeModules(ctx);

  // ---- Write PDB ----
  ctx.stamp("phase: write PDB (LLVM streams)");
  std::string err;
  if (!rsm2pdb::pdb::writePdb(ctx.output_pdb, ctx.inputs, err)) {
    std::fprintf(stderr, "error: PDB write failed: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stdout, "wrote PDB: %s\n", ctx.output_pdb.c_str());

  // ---- Sibling .natstepfilter ----
  if (!ctx.thunks_to_emit.empty()) {
    const std::filesystem::path nspath =
        std::filesystem::path(ctx.output_pdb)
            .replace_extension(".natstepfilter");
    std::ofstream f(nspath, std::ios::binary | std::ios::trunc);
    if (f) {
      f << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        << "<StepFilter xmlns=\"http://schemas.microsoft.com/"
           "vstudio/debugger/natstepfilter/2010\">\n"
        << "    <Function>\n"
        << "        <Name>.*\\$Adjust_[0-9A-Fa-f]+$</Name>\n"
        << "        <Action>NoStepInto</Action>\n"
        << "    </Function>\n"
        << "</StepFilter>\n";
      std::fprintf(stdout, "wrote natstepfilter: %s (%zu adjuster thunks)\n",
                   nspath.string().c_str(), ctx.thunks_to_emit.size());
    } else {
      std::fprintf(stderr, "warning: couldn't write natstepfilter to %s\n",
                   nspath.string().c_str());
    }
  }

  // ---- Inject RSDS pointer into PE ----
  ctx.stamp("phase: inject RSDS into PE");
  const std::string pdb_basename =
      std::filesystem::path(ctx.output_pdb).filename().string();
  if (!rsm2pdb::pe::injectPdbReferenceFile(ctx.input_exe, ctx.inputs.guid,
                                           ctx.inputs.age, pdb_basename,
                                           ctx.output_exe, err)) {
    std::fprintf(stderr, "error: PE injection failed: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stdout, "wrote PE with RSDS pointer: %s\n",
               ctx.output_exe.c_str());
  ctx.stamp("done");
  return 0;
}

} // namespace rsm2pdb::cli
