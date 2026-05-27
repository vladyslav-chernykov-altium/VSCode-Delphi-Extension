#include "pdb/pdb_writer.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/DebugChecksumsSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/DebugStringTableSubsection.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/CodeView/Line.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/SymbolSerializer.h"
#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptorBuilder.h"
#include "llvm/DebugInfo/PDB/Native/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/GSIStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/InfoStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTableBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstring>

namespace rsm2pdb::pdb {

namespace {

// Materialise our CoffSection list into the LLVM-native packed
// `coff_section` layout (== IMAGE_SECTION_HEADER on disk). The
// SectionMap entries DbiStreamBuilder writes derive solely from this
// data so the debugger can map (segment, offset) -> RVA.
std::vector<llvm::object::coff_section>
toLlvmCoffSections(const std::vector<CoffSection>& src) {
    std::vector<llvm::object::coff_section> dst(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) {
        auto& d = dst[i];
        std::memset(&d, 0, sizeof(d));
        const auto& s = src[i];
        const std::size_t n = std::min<std::size_t>(s.name.size(),
                                                    sizeof(d.Name));
        std::memcpy(d.Name, s.name.data(), n);
        d.VirtualSize        = s.virtual_size;
        d.VirtualAddress     = s.virtual_address;
        d.SizeOfRawData      = s.size_of_raw_data;
        d.PointerToRawData   = s.pointer_to_raw_data;
        d.Characteristics    = s.characteristics;
    }
    return dst;
}

} // namespace


bool writePdb(const std::string& path,
              const PdbInputs& inputs,
              std::string& error_out) {
    using namespace llvm;
    using namespace llvm::pdb;

    BumpPtrAllocator alloc;
    PDBFileBuilder builder(alloc);

    if (auto e = builder.initialize(4096)) {
        error_out = "PDBFileBuilder::initialize: " + toString(std::move(e));
        return false;
    }

    // Reserve well-known stream slots (Old MSF Dir / Info / TPI / DBI /
    // IPI). PDBFileBuilder::initialize() doesn't allocate these and
    // commit() fails with "stream too short" without them.
    for (std::uint32_t i = 0; i < kSpecialStreamCount; ++i) {
        if (auto e = builder.getMsfBuilder().addStream(0).takeError()) {
            error_out = "MSFBuilder::addStream reserved: " +
                        toString(std::move(e));
            return false;
        }
    }

    // -- Info stream
    auto& info = builder.getInfoBuilder();
    info.setVersion(PdbImplVC70);
    info.setHashPDBContentsToGUID(false);
    info.setAge(inputs.age);
    codeview::GUID cv_guid{};
    std::memcpy(&cv_guid.Guid, inputs.guid.data(), inputs.guid.size());
    info.setGuid(cv_guid);
    info.addFeature(PdbRaw_FeatureSig::VC140);

    // -- DBI stream
    auto& dbi = builder.getDbiBuilder();
    dbi.setVersionHeader(PdbDbiV70);
    dbi.setAge(static_cast<std::uint16_t>(inputs.age));
    dbi.setMachineType(COFF::IMAGE_FILE_MACHINE_AMD64);
    dbi.setBuildNumber(14, 11);   // pretend to be VS2017+; many debuggers
                                  // refuse to load PDBs with version 0.

    // -- Empty TPI + IPI (still need version headers)
    builder.getTpiBuilder().setVersionHeader(PdbTpiV80);
    builder.getIpiBuilder().setVersionHeader(PdbTpiV80);

    // -- GSI: publish caller-supplied publics
    auto& gsi = builder.getGsiBuilder();

    std::vector<BulkPublic> bulks;
    bulks.reserve(inputs.publics.size());
    for (const auto& p : inputs.publics) {
        BulkPublic b{};
        b.Name    = p.name.c_str();
        b.NameLen = static_cast<std::uint32_t>(p.name.size());
        b.Segment = p.segment;
        b.Offset  = p.offset;
        b.setFlags(p.is_function
                       ? codeview::PublicSymFlags::Function
                       : codeview::PublicSymFlags::None);
        bulks.push_back(b);
    }
    if (!bulks.empty()) {
        gsi.addPublicSymbols(std::move(bulks));
    }

    // For each non-function public, also emit an S_GDATA32 into the
    // globals stream so cppvsdbg / WinDbg surface the variable in the
    // Watch view. Two reasons we don't use the .map's fully-qualified
    // Pascal name here:
    //   (1) cppvsdbg's expression evaluator treats `.` as field access
    //       (Pascal's namespace separator is a syntax error to it), so
    //       `Watch: two_units.S` reports "two_units undefined".
    //   (2) Without a real type the debugger can't display the value,
    //       so we default everything to `void*` -- 8-byte hex view
    //       that doubles as a freely-castable handle in Watch
    //       (e.g. `(char*)PChar, 10` shows 10 chars; `*(int*)&I`
    //       reads 4 bytes at a variable's address).
    // The fully-qualified name still lives in S_PUB32 for stack traces.
    // Stored strings must outlive gsi.addGlobalSymbol() so we hold them
    // in a vector kept alive until commit().
    std::vector<std::string> global_unqualified;
    global_unqualified.reserve(inputs.publics.size());
    for (const auto& p : inputs.publics) {
        if (p.is_function) continue;
        const auto dot = p.name.find_last_of('.');
        global_unqualified.push_back(
            dot == std::string::npos ? p.name : p.name.substr(dot + 1));
    }
    {
        std::size_t idx = 0;
        for (const auto& p : inputs.publics) {
            if (p.is_function) continue;
            codeview::DataSym data(codeview::SymbolRecordKind::GlobalData);
            data.Type       = codeview::TypeIndex::VoidPointer64();
            data.DataOffset = p.offset;
            data.Segment    = p.segment;
            data.Name       = global_unqualified[idx++];
            gsi.addGlobalSymbol(data);
        }
    }

    dbi.setPublicsStreamIndex(gsi.getPublicsStreamIndex());
    dbi.setGlobalsStreamIndex(gsi.getGlobalsStreamIndex());
    dbi.setSymbolRecordStreamIndex(gsi.getRecordStreamIndex());

    // SectionMap (derived from PE section headers). PUBSYM resolution
    // relies on this; without it the debugger can't translate
    // (segment, offset) -> RVA at lookup time.
    const auto llvm_sections = toLlvmCoffSections(inputs.sections);
    dbi.createSectionMap(llvm_sections);

    // Write the COFF section headers verbatim into the PDB's
    // DbgStream[SectionHdr]. vsdbg / WinDbg use this to convert the
    // (segment, offset) pairs stored in symbols + line tables into
    // module-relative RVAs. Without it, vsdbg raises "Unexpected
    // symbol reader error" when resolving a breakpoint to a PC.
    if (!llvm_sections.empty()) {
        ArrayRef<std::uint8_t> hdr_bytes(
            reinterpret_cast<const std::uint8_t*>(llvm_sections.data()),
            llvm_sections.size() * sizeof(object::coff_section));
        if (auto e = dbi.addDbgStream(DbgHeaderType::SectionHdr, hdr_bytes)) {
            error_out = "DbiStreamBuilder::addDbgStream(SectionHdr): " +
                        toString(std::move(e));
            return false;
        }
    }

    // -- Modules: one DBI module per Pascal compile unit. Each carries
    //    S_GPROC32 / S_END procedure symbols and a C13 line subsection
    //    so the debugger can map VAs to source lines for breakpoints.
    //
    //    All modules share one DebugStringTableSubsection so the
    //    PDB-global /names stream resolves every file path correctly.
    //    lld follows the same pattern.
    auto shared_strings =
        std::make_shared<codeview::DebugStringTableSubsection>();

    // Helper: look up section Characteristics by 1-based segment index
    // so each SectionContrib carries the same flags as the PE section.
    auto sectionCharacteristics = [&](std::uint16_t seg) -> std::uint32_t {
        if (seg == 0 || seg > inputs.sections.size()) return 0;
        return inputs.sections[seg - 1].characteristics;
    };

    // Section contributions: collected per-function and sorted by
    // (Section, Offset) before submission. PDB spec requires this
    // ordering so debuggers can binary-search. Without sorting,
    // vsdbg fails to resolve breakpoints for functions whose
    // contribs land "before" earlier-listed ones (manifested as: BP
    // works only in some source files).
    std::vector<SectionContrib> pending_contribs;

    std::uint16_t mod_index = 0;
    for (const auto& mod : inputs.modules) {
        auto m_or_err = dbi.addModuleInfo(mod.name);
        if (!m_or_err) {
            error_out = "DbiStreamBuilder::addModuleInfo: " +
                        toString(m_or_err.takeError());
            return false;
        }
        auto& m = *m_or_err;
        m.setObjFileName(mod.name + ".obj");
        for (const auto& src : mod.sources) {
            if (src.source_path.empty()) continue;
            if (auto e = dbi.addModuleSourceFile(m, src.source_path)) {
                error_out = "DbiStreamBuilder::addModuleSourceFile: " +
                            toString(std::move(e));
                return false;
            }
        }

        // S_GPROC32 + S_FRAMEPROC + S_REGREL32 (params, locals) + S_END.
        // ProcSym.End is patched to point at the matching S_END's
        // stream offset; otherwise the scope is "open" and vsdbg
        // won't associate locals with the function.
        for (const auto& fn : mod.functions) {
            const std::uint32_t proc_offset = m.getNextSymbolOffset();

            codeview::ProcSym proc(
                codeview::SymbolRecordKind::GlobalProcSym);
            proc.Parent      = 0;
            proc.End         = 0;       // patched below via raw bytes
            proc.Next        = 0;
            proc.CodeSize    = fn.size;
            proc.DbgStart    = 0;
            proc.DbgEnd      = fn.size;
            proc.FunctionType = codeview::TypeIndex::None();
            proc.CodeOffset  = fn.offset;
            proc.Segment     = fn.segment;
            proc.Flags       = codeview::ProcSymFlags::None;
            proc.Name        = fn.name;
            auto cv_proc = codeview::SymbolSerializer::writeOneSymbol(
                proc, alloc, codeview::CodeViewContainer::Pdb);
            // The serializer wrote into a buffer owned by `alloc`.
            // Grab a writable pointer so we can patch End later.
            std::uint8_t* proc_bytes = const_cast<std::uint8_t*>(
                cv_proc.data().data());
            m.addSymbol(cv_proc);

            // S_FRAMEPROC: tells vsdbg which register holds the frame
            // pointer for locals + params. Without this, the
            // S_REGREL32 records below are written but vsdbg can't
            // resolve them, so Locals stays empty.
            // Encoding: bits 14-15 = local FP, 16-17 = param FP;
            // EncodedFramePtrReg::FramePtr (= 2) maps to RBP on x64.
            codeview::FrameProcSym frame(
                codeview::SymbolRecordKind::FrameProcSym);
            frame.TotalFrameBytes           = fn.size;
            frame.PaddingFrameBytes         = 0;
            frame.OffsetToPadding           = 0;
            frame.BytesOfCalleeSavedRegisters = 0;
            frame.OffsetOfExceptionHandler  = 0;
            frame.SectionIdOfExceptionHandler = 0;
            frame.Flags = static_cast<codeview::FrameProcedureOptions>(
                (2u << 14) | (2u << 16));
            m.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
                frame, alloc, codeview::CodeViewContainer::Pdb));

            for (const auto& v : fn.locals) {
                if (v.register_id != 0) {
                    // Variable lives in a CPU register for the whole
                    // function range. Emit S_LOCAL + S_DEFRANGE_REGISTER
                    // covering [fn.offset, fn.offset + fn.size).
                    codeview::LocalSym loc(
                        codeview::SymbolRecordKind::LocalSym);
                    loc.Type  = codeview::TypeIndex::VoidPointer64();
                    loc.Flags = v.is_param
                                ? codeview::LocalSymFlags::IsParameter
                                : codeview::LocalSymFlags::None;
                    loc.Name  = v.name;
                    m.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
                        loc, alloc, codeview::CodeViewContainer::Pdb));

                    codeview::DefRangeRegisterSym dr(
                        codeview::SymbolRecordKind::DefRangeRegisterSym);
                    dr.Hdr.Register = v.register_id;
                    dr.Hdr.MayHaveNoName = 0;
                    dr.Range.OffsetStart = fn.offset;
                    dr.Range.ISectStart  = fn.segment;
                    dr.Range.Range = static_cast<std::uint16_t>(
                        std::min<std::uint32_t>(fn.size,
                            codeview::MaxDefRange));
                    m.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
                        dr, alloc, codeview::CodeViewContainer::Pdb));
                    continue;
                }
                if (v.optimized_out) {
                    // S_LOCAL alone (no S_DEFRANGE_*) tells the
                    // debugger the variable exists at this scope but
                    // has no known runtime location. cppvsdbg surfaces
                    // it as `<optimized away>` in Locals.
                    codeview::LocalSym loc(
                        codeview::SymbolRecordKind::LocalSym);
                    loc.Type  = codeview::TypeIndex::VoidPointer64();
                    loc.Flags = v.is_param
                                ? codeview::LocalSymFlags::IsParameter
                                : codeview::LocalSymFlags::None;
                    loc.Name  = v.name;
                    m.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
                        loc, alloc, codeview::CodeViewContainer::Pdb));
                    continue;
                }
                codeview::RegRelativeSym reg(
                    codeview::SymbolRecordKind::RegRelativeSym);
                reg.Offset   = static_cast<std::uint32_t>(v.offset);
                reg.Type     = codeview::TypeIndex::VoidPointer64();
                reg.Register = codeview::RegisterId::RBP;
                reg.Name     = v.name;
                m.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
                    reg, alloc, codeview::CodeViewContainer::Pdb));
            }

            // S_END is about to land at this offset; patch the proc's
            // End field now (it's at byte offset 8 = 4-byte CVHeader
            // + 4-byte Parent field).
            const std::uint32_t end_offset = m.getNextSymbolOffset();
            std::memcpy(proc_bytes + 8, &end_offset, 4);
            (void)proc_offset;  // proc_offset == module_offset of proc,
                                // useful for debugging if needed.

            codeview::ScopeEndSym end_sym(
                codeview::SymbolRecordKind::ScopeEndSym);
            m.addSymbol(codeview::SymbolSerializer::writeOneSymbol(
                end_sym, alloc, codeview::CodeViewContainer::Pdb));

            SectionContrib sc{};
            sc.ISect          = fn.segment;
            sc.Off            = fn.offset;
            sc.Size           = fn.size;
            sc.Characteristics = sectionCharacteristics(fn.segment);
            sc.Imod           = mod_index;
            sc.DataCrc        = 0;
            sc.RelocCrc       = 0;
            pending_contribs.push_back(sc);
        }

        // C13 line info. Skip cleanly when there's nothing to emit --
        // but first bump mod_index so the next module's SectionContribs
        // get the right Imod. (Forgetting this collapsed every
        // line-less module's contribs onto a single bogus Imod.)
        bool any_lines = false;
        for (const auto& src : mod.sources) {
            if (!src.lines.empty() && !src.source_path.empty()) {
                any_lines = true; break;
            }
        }
        if (!any_lines) { ++mod_index; continue; }

        // One DebugChecksumsSubsection per module holds an entry for
        // every source file referenced by this module's line tables.
        // cppvsdbg refuses to resolve BPs in modules whose checksums
        // are FileChecksumKind::None, so we compute MD5 of each path
        // (zero-filled MD5 when the file is unreadable -- BPs still
        // bind, just with a "source changed" warning).
        auto checksums = std::make_shared<codeview::DebugChecksumsSubsection>(
            *shared_strings);
        for (const auto& src : mod.sources) {
            if (src.source_path.empty()) continue;
            std::array<std::uint8_t, 16> md5{};
            if (auto buf_or = MemoryBuffer::getFile(src.source_path)) {
                MD5 hasher;
                hasher.update((*buf_or)->getBuffer());
                MD5::MD5Result result;
                hasher.final(result);
                std::memcpy(md5.data(), result.data(), 16);
            }
            checksums->addChecksum(src.source_path,
                                   codeview::FileChecksumKind::MD5,
                                   md5);
        }
        m.addDebugSubsection(checksums);

        // Build a per-source sorted index of line entries so we can
        // binary-search by (segment, offset) for each function instead
        // of scanning every line every time. Skipping this turned the
        // composition phase O(F * L) per module which on AdvPCB (~336k
        // functions / ~1.8M line entries) burned minutes.
        struct LineIdx {
            const ModuleSource* src;
            const ModuleLine*   line;
        };
        std::vector<LineIdx> sorted_lines;
        std::size_t total_lines = 0;
        for (const auto& s : mod.sources) total_lines += s.lines.size();
        sorted_lines.reserve(total_lines);
        for (const auto& s : mod.sources) {
            for (const auto& l : s.lines) sorted_lines.push_back({&s, &l});
        }
        std::sort(sorted_lines.begin(), sorted_lines.end(),
            [](const LineIdx& a, const LineIdx& b) {
                if (a.line->segment != b.line->segment)
                    return a.line->segment < b.line->segment;
                return a.line->offset < b.line->offset;
            });

        // One DebugLinesSubsection per function. lower_bound jumps us
        // to the first line at or after fn.offset; iterate until we
        // run past the function's end. A function's lines are assumed
        // to live in one source file (true in practice for Delphi).
        for (const auto& fn : mod.functions) {
            ModuleLine key{};
            key.segment = fn.segment;
            key.offset  = fn.offset;
            auto first = std::lower_bound(
                sorted_lines.begin(), sorted_lines.end(), key,
                [](const LineIdx& a, const ModuleLine& k) {
                    if (a.line->segment != k.segment)
                        return a.line->segment < k.segment;
                    return a.line->offset < k.offset;
                });
            const std::uint32_t fn_end =
                fn.offset + std::max<std::uint32_t>(fn.size, 1);
            const ModuleSource* fn_src = nullptr;
            std::vector<const ModuleLine*> fn_lines;
            for (auto it = first; it != sorted_lines.end(); ++it) {
                if (it->line->segment != fn.segment) break;
                if (it->line->offset >= fn_end) break;
                if (!fn_src) fn_src = it->src;
                fn_lines.push_back(it->line);
            }
            if (fn_lines.empty()) continue;

            auto lines = std::make_shared<codeview::DebugLinesSubsection>(
                *checksums, *shared_strings);
            lines->setRelocationAddress(fn.segment, fn.offset);
            lines->setCodeSize(fn.size);
            lines->setFlags(codeview::LineFlags::LF_None);
            lines->createBlock(fn_src->source_path);
            for (const auto* l : fn_lines) {
                lines->addLineInfo(
                    l->offset - fn.offset,
                    codeview::LineInfo(l->line, l->line, /*isStmt*/true));
            }
            m.addDebugSubsection(lines);
        }
        ++mod_index;
    }

    // Submit collected section contributions in (ISect, Off) order.
    std::sort(pending_contribs.begin(), pending_contribs.end(),
        [](const SectionContrib& a, const SectionContrib& b) {
            if (a.ISect != b.ISect) return a.ISect < b.ISect;
            return a.Off < b.Off;
        });
    for (const auto& sc : pending_contribs) dbi.addSectionContrib(sc);

    // Publish the shared string table into the PDB-global /names
    // stream once, after every module has registered its file paths.
    builder.getStringTableBuilder().setStrings(*shared_strings);

    codeview::GUID out_guid{};
    if (auto e = builder.commit(path, &out_guid)) {
        error_out = "PDBFileBuilder::commit: " + toString(std::move(e));
        return false;
    }
    return true;
}

} // namespace rsm2pdb::pdb
