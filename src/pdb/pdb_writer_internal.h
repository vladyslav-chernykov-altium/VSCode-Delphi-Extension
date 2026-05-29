#pragma once

// Internal: PdbWriter class declaration + shared helpers used
// by the pdb_writer_*.cpp submodules. Public clients consume
// pdb_writer.h (writePdb + input structs) only.

#include "pdb/pdb_writer.h"

#include "llvm/DebugInfo/CodeView/AppendingTypeTableBuilder.h"
#include "llvm/DebugInfo/CodeView/DebugStringTableSubsection.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Allocator.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rsm2pdb::pdb::detail {

// Materialise our CoffSection list into the LLVM-native packed
// `coff_section` layout (== IMAGE_SECTION_HEADER on disk).
inline std::vector<llvm::object::coff_section>
toLlvmCoffSections(const std::vector<CoffSection>& src) {
    std::vector<llvm::object::coff_section> dst(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        auto& d = dst[i];
        std::memset(&d, 0, sizeof(d));
        const auto& s = src[i];
        const std::size_t n = std::min<std::size_t>(s.name.size(),
                                                    sizeof(d.Name));
        std::memcpy(d.Name, s.name.data(), n);
        d.VirtualSize      = s.virtual_size;
        d.VirtualAddress   = s.virtual_address;
        d.SizeOfRawData    = s.size_of_raw_data;
        d.PointerToRawData = s.pointer_to_raw_data;
        d.Characteristics  = s.characteristics;
    }
    return dst;
}

class PdbWriter {
public:
  PdbWriter(const std::string &path, const PdbInputs &inputs,
            std::string &error_out)
      : path_(path), inputs_(inputs), error_out_(error_out), builder_(alloc_),
        tpi_table_(alloc_) {}
  bool run();

private:
  // Phase helpers. typeForSize / typeForKindParts route a (byte_size,
  // optional prim_kind) tuple to the right CodeView TypeIndex. They
  // are shared by aggregate, public and module emission.
  llvm::codeview::TypeIndex typeForSize(std::uint32_t size);
  llvm::codeview::TypeIndex
  typeForKindParts(std::uint32_t byte_size,
                   const std::optional<model::PrimitiveKind> &prim_kind);

  // Phase D + E: emit a TPI record for every user aggregate (record,
  // class, enum, set). Populates aggregate_inner_ti_ /
  // aggregate_var_ti_ so subsequent phases (publics, locals) can
  // resolve variable / param TypeIndices through aggregate_index.
  void emitAggregates();

  // Route a (byte_size, optional prim_kind, optional aggregate_index)
  // tuple to the right CodeView TypeIndex. aggregate_index wins when
  // set (resolves through aggregate_var_ti_); otherwise falls back to
  // typeForKindParts.
  llvm::codeview::TypeIndex
  resolveTypeIndex(std::uint32_t byte_size,
                   const std::optional<model::PrimitiveKind> &prim_kind,
                   const std::optional<std::size_t> &aggregate_index);

  // Phase: GSI publics (S_PUB32) + S_GDATA32 globals + DBI stream-
  // index wiring. Reads inputs_.publics; appends unqualified copies
  // to global_unqualified_ (kept alive until commit).
  void emitPublicsAndGlobals();

  // Phase: one DBI module per Pascal compile unit. Emits S_GPROC32
  // / S_FRAMEPROC / S_REGREL32 / S_END for each function, plus C13
  // FileChecksums + DebugLinesSubsection for each source. Also
  // collects + sorts SectionContribs (PDB spec requires (ISect,
  // Off) order for debugger binary-search). Populates
  // shared_strings_ which the caller publishes to the PDB-global
  // /names stream after this returns.
  //
  // Returns false (with error_out_ filled) on DbiBuilder failure.
  bool emitModules();

  const std::string &path_;
  const PdbInputs &inputs_;
  std::string &error_out_;

  // Backing state shared across the phase methods.
  llvm::BumpPtrAllocator alloc_;
  llvm::pdb::PDBFileBuilder builder_;
  llvm::codeview::AppendingTypeTableBuilder tpi_table_;
  // Memoised LF_ARRAY records for non-{1,2,4,8}-byte sizes; keyed by
  // size in bytes. Populated lazily by typeForSize().
  std::unordered_map<std::uint32_t, llvm::codeview::TypeIndex>
      byte_array_cache_;
  // Parallel vectors built by emitAggregates(), one entry per
  // PdbInputs::aggregates element in the same order.
  //   inner_ti -- the LF_STRUCTURE / LF_CLASS / LF_ENUM itself,
  //               used for LF_BCLASS base references + LF_MEMBER
  //               nested-record fields.
  //   var_ti   -- what a Variable / public references:
  //                 records / enums / sets == inner_ti (value type)
  //                 classes                == LF_POINTER -> inner_ti
  std::vector<llvm::codeview::TypeIndex> aggregate_inner_ti_;
  std::vector<llvm::codeview::TypeIndex> aggregate_var_ti_;
  // FE.1: LF_MFUNCTION TypeIndex per registered class method, keyed
  // by qualified function name ("unit.Class.Method"). Populated by
  // emitAggregates from AggregateRecord::methods; consumed by
  // emitModules to wire S_GPROC32.FunctionType.
  std::unordered_map<std::string, llvm::codeview::TypeIndex>
      method_type_by_qname_;
  // FE.1: C++-scoped names (`TDog::GetBarkCount`) emitted as
  // additional S_PUB32 entries alongside the Pascal-dot S_GPROC32.
  // cppvsdbg's expression evaluator constructs the expected symbol
  // name from class scope (`<Class>::<Method>`) when resolving
  // `obj.method()` calls; without a matching public it reports
  // "Function has no address". The strings must outlive
  // gsi.addPublicSymbols() until commit(), hence the member-side
  // backing store.
  std::vector<std::string> method_scoped_names_;
  // Stripped-of-prefix names for S_GDATA32 globals (cppvsdbg parses
  // `.` as field access so `Watch: two_units.S` fails -- we emit the
  // unqualified `S` instead). The strings must outlive
  // gsi.addGlobalSymbol() until commit(), hence the member-side
  // backing store.
  std::vector<std::string> global_unqualified_;
  // Shared string table for per-module DebugStringTableSubsection
  // entries; populated by emitModules() and published into the
  // PDB-global /names stream by run() after emitModules() returns.
  std::shared_ptr<llvm::codeview::DebugStringTableSubsection> shared_strings_;
};

} // namespace rsm2pdb::pdb::detail
