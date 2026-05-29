// PDB writer entry point. Thin wrapper over the PdbWriter class
// declared in pdb_writer_internal.h. Phase methods live in
// pdb_writer_types.cpp / _symbols.cpp / _modules.cpp; only
// run() + the wrapper stay here.

#include "pdb/pdb_writer.h"
#include "pdb/pdb_writer_internal.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/DebugInfo/PDB/Native/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/InfoStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTableBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstring>

namespace rsm2pdb::pdb {

namespace { using detail::PdbWriter; }

bool PdbWriter::run() {
  using namespace llvm;
  using namespace llvm::pdb;

  if (auto e = builder_.initialize(4096)) {
    error_out_ = "PDBFileBuilder::initialize: " + toString(std::move(e));
    return false;
  }

  // Reserve well-known stream slots (Old MSF Dir / Info / TPI / DBI /
  // IPI). PDBFileBuilder::initialize() doesn't allocate these and
  // commit() fails with "stream too short" without them.
  for (std::uint32_t i = 0; i < kSpecialStreamCount; ++i) {
    if (auto e = builder_.getMsfBuilder().addStream(0).takeError()) {
      error_out_ = "MSFBuilder::addStream reserved: " + toString(std::move(e));
      return false;
    }
  }

  // -- Info stream
  auto &info = builder_.getInfoBuilder();
  info.setVersion(PdbImplVC70);
  info.setHashPDBContentsToGUID(false);
  info.setAge(inputs_.age);
  codeview::GUID cv_guid{};
  std::memcpy(&cv_guid.Guid, inputs_.guid.data(), inputs_.guid.size());
  info.setGuid(cv_guid);
  info.addFeature(PdbRaw_FeatureSig::VC140);

  // -- DBI stream
  auto &dbi = builder_.getDbiBuilder();
  dbi.setVersionHeader(PdbDbiV70);
  dbi.setAge(static_cast<std::uint16_t>(inputs_.age));
  dbi.setMachineType(COFF::IMAGE_FILE_MACHINE_AMD64);
  dbi.setBuildNumber(14, 11); // pretend to be VS2017+; many debuggers
                              // refuse to load PDBs with version 0.

  // -- TPI + IPI: IPI stays empty; TPI receives any LF_ARRAY records
  //    we build for non-{1,2,4,8}-byte variables. Built up as we walk
  //    modules, then bulk-pushed below.
  builder_.getTpiBuilder().setVersionHeader(PdbTpiV80);
  builder_.getIpiBuilder().setVersionHeader(PdbTpiV80);

  emitAggregates();

  // SectionMap (derived from PE section headers). PUBSYM resolution
  // relies on this; without it the debugger can't translate
  // (segment, offset) -> RVA at lookup time.
  const auto llvm_sections = toLlvmCoffSections(inputs_.sections);
  dbi.createSectionMap(llvm_sections);

  // Write the COFF section headers verbatim into the PDB's
  // DbgStream[SectionHdr]. vsdbg / WinDbg use this to convert the
  // (segment, offset) pairs stored in symbols + line tables into
  // module-relative RVAs. Without it, vsdbg raises "Unexpected
  // symbol reader error" when resolving a breakpoint to a PC.
  if (!llvm_sections.empty()) {
    ArrayRef<std::uint8_t> hdr_bytes(
        reinterpret_cast<const std::uint8_t *>(llvm_sections.data()),
        llvm_sections.size() * sizeof(object::coff_section));
    if (auto e = dbi.addDbgStream(DbgHeaderType::SectionHdr, hdr_bytes)) {
      error_out_ = "DbiStreamBuilder::addDbgStream(SectionHdr): " +
                   toString(std::move(e));
      return false;
    }
  }

  // FE.1.5: emitModules must run BEFORE emitPublicsAndGlobals so
  // that scoped method publics (`TDog::GetBarkCount`) collected
  // during S_GPROC32 emission can be batched into the single
  // gsi.addPublicSymbols() call. LLVM's GSIStreamBuilder asserts
  // that addPublicSymbols is called at most once; calling it
  // per-method (the FE.1 approach) silently dropped all but the
  // last scoped public in release builds.
  if (!emitModules())
    return false;

  emitPublicsAndGlobals();

  // Push any LF_ARRAY records (byte[N] for non-{1,2,4,8} variable
  // widths) into the TPI stream so the symbol-level TypeIndex
  // references we emitted above resolve at debug time.
  for (auto rec : tpi_table_.records()) {
    builder_.getTpiBuilder().addTypeRecord(rec, std::nullopt);
  }

  // Publish the shared string table into the PDB-global /names
  // stream once, after every module has registered its file paths.
  builder_.getStringTableBuilder().setStrings(*shared_strings_);

  // Embed NatVis XML as a PDB injected source. VS native + cppvsdbg
  // auto-load NatVis from injected sources at debug time; this
  // bypasses the launch.json `visualizerFile` requirement that
  // cppvsdbg-in-VSCode otherwise needs, and matches how MSVC
  // link.exe's /natvis: flow stores natvis content. The "name"
  // shows up in dia2dump / llvm-pdbutil dump --injected-sources.
  if (!inputs_.natvis_xml.empty()) {
    auto buf = MemoryBuffer::getMemBufferCopy(inputs_.natvis_xml,
                                              "rsm2pdb.natvis");
    builder_.addInjectedSource("rsm2pdb.natvis", std::move(buf));
  }

  codeview::GUID out_guid{};
  if (auto e = builder_.commit(path_, &out_guid)) {
    error_out_ = "PDBFileBuilder::commit: " + toString(std::move(e));
    return false;
  }
  return true;
}

bool writePdb(const std::string &path, const PdbInputs &inputs,
              std::string &error_out) {
  return PdbWriter(path, inputs, error_out).run();
}

} // namespace rsm2pdb::pdb
