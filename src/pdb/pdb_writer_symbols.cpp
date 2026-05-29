#include "pdb/pdb_writer.h"
#include "pdb/pdb_writer_internal.h"

#include "llvm/ADT/APSInt.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/ContinuationRecordBuilder.h"
#include "llvm/DebugInfo/CodeView/DebugChecksumsSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/CodeView/Line.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolSerializer.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptorBuilder.h"
#include "llvm/DebugInfo/PDB/Native/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/GSIStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace rsm2pdb::pdb::detail {

// Phase: GSI publics (S_PUB32) + S_GDATA32 globals.
//
// For each non-function public we ALSO emit an S_GDATA32 into the
// globals stream so cppvsdbg / WinDbg surface the variable in Watch.
// The S_GDATA32 name is stripped of its module prefix (`two_units.S`
// -> `S`) because cppvsdbg's expression evaluator treats `.` as field
// access and would otherwise report "two_units undefined". The
// fully-qualified name still lives in S_PUB32 for stack traces.
//
// Stored strings must outlive gsi.addGlobalSymbol() so they're held
// in global_unqualified_ on the writer (kept alive until commit()).
void PdbWriter::emitPublicsAndGlobals() {
  using namespace llvm;
  using namespace llvm::pdb;

  auto &dbi = builder_.getDbiBuilder();
  auto &gsi = builder_.getGsiBuilder();

  std::vector<BulkPublic> bulks;
  bulks.reserve(inputs_.publics.size() + pending_scoped_publics_.size());
  for (const auto &p : inputs_.publics) {
    BulkPublic b{};
    b.Name = p.name.c_str();
    b.NameLen = static_cast<std::uint32_t>(p.name.size());
    b.Segment = p.segment;
    b.Offset = p.offset;
    b.setFlags(p.is_function ? codeview::PublicSymFlags::Function
                             : codeview::PublicSymFlags::None);
    bulks.push_back(b);
  }
  // FE.1.5: append the C++-scoped method publics collected during
  // emitModules(). Their backing strings live in
  // method_scoped_names_ (reserved to its final size before the
  // loop, so .c_str() pointers in pending_scoped_publics_ remain
  // valid). One bulk call satisfies LLVM's addPublicSymbols
  // assert-once contract.
  for (const auto &b : pending_scoped_publics_)
    bulks.push_back(b);
  if (!bulks.empty()) {
    gsi.addPublicSymbols(std::move(bulks));
  }

  global_unqualified_.reserve(inputs_.publics.size());
  for (const auto &p : inputs_.publics) {
    if (p.is_function)
      continue;
    const auto dot = p.name.find_last_of('.');
    global_unqualified_.push_back(
        dot == std::string::npos ? p.name : p.name.substr(dot + 1));
  }
  std::size_t idx = 0;
  for (const auto &p : inputs_.publics) {
    if (p.is_function)
      continue;
    codeview::DataSym data(codeview::SymbolRecordKind::GlobalData);
    data.Type = resolveTypeIndex(p.byte_size, p.prim_kind, p.aggregate_index);
    data.DataOffset = p.offset;
    data.Segment = p.segment;
    data.Name = global_unqualified_[idx++];
    gsi.addGlobalSymbol(data);
  }

  dbi.setPublicsStreamIndex(gsi.getPublicsStreamIndex());
  dbi.setGlobalsStreamIndex(gsi.getGlobalsStreamIndex());
  dbi.setSymbolRecordStreamIndex(gsi.getRecordStreamIndex());
}

} // namespace rsm2pdb::pdb::detail
