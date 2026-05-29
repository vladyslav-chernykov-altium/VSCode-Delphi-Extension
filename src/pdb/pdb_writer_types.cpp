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

// Pick a CodeView TypeIndex for a stack variable of `size` bytes.
//   1/2/4/8 -> simple built-in (UCHAR/USHORT/UINT32/UINT64) so the
//              debugger displays them as plain hex of the right width;
//   N other -> LF_ARRAY of UCHAR with that count, cached per size;
//   0       -> void* (8-byte hex), our "unknown size" fallback.
llvm::codeview::TypeIndex PdbWriter::typeForSize(std::uint32_t size) {
  using namespace llvm;
  switch (size) {
  case 0:
    return codeview::TypeIndex::VoidPointer64();
  case 1:
    return codeview::TypeIndex::UnsignedCharacter();
  case 2:
    return codeview::TypeIndex::UInt16Short();
  case 4:
    return codeview::TypeIndex::UInt32();
  case 8:
    return codeview::TypeIndex::UInt64();
  default:
    break;
  }
  auto it = byte_array_cache_.find(size);
  if (it != byte_array_cache_.end())
    return it->second;
  codeview::ArrayRecord rec(codeview::TypeIndex::UnsignedCharacter(),
                            codeview::TypeIndex::UInt32(), size, std::string{});
  const auto ti = tpi_table_.writeLeafType(rec);
  byte_array_cache_.emplace(size, ti);
  return ti;
}

// Pick a CodeView TypeIndex for a Pascal primitive kind. Falls back to
// size-based selection when the kind is unset or doesn't map cleanly
// (e.g. Float80 which CodeView doesn't have a simple type for -- emit
// as 10-byte array).
llvm::codeview::TypeIndex PdbWriter::typeForKindParts(
    std::uint32_t byte_size,
    const std::optional<model::PrimitiveKind> &prim_kind) {
  using namespace llvm;
  if (!prim_kind)
    return typeForSize(byte_size);
  using K = model::PrimitiveKind;
  switch (*prim_kind) {
  // Booleans: CodeView's Boolean8/16/32 simple types render as
  // `true`/`false` rather than 0/1, matching Pascal's expected
  // display.
  case K::Bool:
    switch (byte_size) {
    case 1:
      return codeview::TypeIndex(codeview::SimpleTypeKind::Boolean8);
    case 2:
      return codeview::TypeIndex(codeview::SimpleTypeKind::Boolean16);
    case 4:
      return codeview::TypeIndex(codeview::SimpleTypeKind::Boolean32);
    case 8:
      return codeview::TypeIndex(codeview::SimpleTypeKind::Boolean64);
    default:
      return typeForSize(byte_size);
    }
  // AnsiChar -> NarrowCharacter (T_CHAR, 1 byte).
  case K::Char:
    return codeview::TypeIndex::NarrowCharacter();
  // Char / WideChar -> WideCharacter (T_WCHAR, 2 byte).
  case K::WChar:
    return codeview::TypeIndex::WideCharacter();
  // Signed integers.
  case K::Int8:
    return codeview::TypeIndex::SignedCharacter();
  case K::Int16:
    return codeview::TypeIndex::Int16Short();
  case K::Int32:
    return codeview::TypeIndex::Int32();
  case K::Int64:
    return codeview::TypeIndex::Int64Quad();
  // Unsigned integers.
  case K::UInt8:
    return codeview::TypeIndex::UnsignedCharacter();
  case K::UInt16:
    return codeview::TypeIndex::UInt16Short();
  case K::UInt32:
    return codeview::TypeIndex::UInt32();
  case K::UInt64:
    return codeview::TypeIndex::UInt64();
  // Floats.
  case K::Float32:
    return codeview::TypeIndex::Float32();
  case K::Float64:
    return codeview::TypeIndex::Float64();
  // Float80 (Pascal Extended on x86) has no CodeView simple type;
  // emit as a 10-byte hex array.
  case K::Float80:
    return typeForSize(byte_size);
  // Pointer-to-char primitives -- CodeView lets us encode these
  // as a SimpleTypeKind paired with a 64-bit pointer mode, no
  // LF_POINTER TPI record needed. cdb / VS render the pointee as
  // a string starting at the address.
  case K::PChar:
    return codeview::TypeIndex(codeview::SimpleTypeKind::NarrowCharacter,
                               codeview::SimpleTypeMode::NearPointer64);
  case K::PWChar:
    return codeview::TypeIndex(codeview::SimpleTypeKind::WideCharacter,
                               codeview::SimpleTypeMode::NearPointer64);
  }
  return typeForSize(byte_size);
}

llvm::codeview::TypeIndex PdbWriter::resolveTypeIndex(
    std::uint32_t byte_size,
    const std::optional<model::PrimitiveKind> &prim_kind,
    const std::optional<std::size_t> &aggregate_index) {
  if (aggregate_index && *aggregate_index < aggregate_var_ti_.size()) {
    return aggregate_var_ti_[*aggregate_index];
  }
  return typeForKindParts(byte_size, prim_kind);
}

// -- LF_FIELDLIST + LF_STRUCTURE / LF_CLASS + LF_POINTER ----------
//
// Phase D + E: emit a TPI record for every user aggregate.
//
//   Record (TPoint, TPerson, TBox, TPacked, TBig, ...)
//     LF_FIELDLIST -- one LF_MEMBER per field.
//     LF_STRUCTURE -- references the field list.
//     A variable of this type references the LF_STRUCTURE
//     directly (records are value types in Pascal).
//
//   Class (TShape, TCircle, TBag, ...)
//     LF_FIELDLIST -- optional LF_BCLASS first, then LF_MEMBERs.
//     LF_CLASS     -- references the field list.
//     LF_POINTER   -- 8-byte Near64 pointer to LF_CLASS.
//     A variable of this type references the LF_POINTER (Pascal
//     class instances live on the heap; `var X: TShape` is a
//     pointer slot, not an inline structure).
//
// We keep two parallel TypeIndex vectors:
//   aggregate_inner_ti -- the LF_STRUCTURE / LF_CLASS itself,
//                         used for LF_BCLASS base references
//                         and LF_MEMBER nested-record fields.
//   aggregate_var_ti   -- what variables / globals reference:
//                         records  == aggregate_inner_ti
//                         classes  == LF_POINTER -> inner_ti
//
// Single-pass emission is correct for Phase D + E because the
// cli_cmd_pdb registrar walks composite refs AND class bases
// depth-first, so dependents (base classes, nested records) are
// always registered before their containers / derived classes.
void PdbWriter::emitAggregates() {
  using namespace llvm;

  aggregate_inner_ti_.reserve(inputs_.aggregates.size());
  aggregate_var_ti_.reserve(inputs_.aggregates.size());

  // Resolve a field's TypeIndex.
  auto fieldTypeIndex = [&](const AggregateField &f) -> codeview::TypeIndex {
    if (f.nested_aggregate) {
      const auto idx = *f.nested_aggregate;
      if (idx < aggregate_var_ti_.size())
        return aggregate_var_ti_[idx];
      // Forward ref not supported; fall back to UCHAR to
      // avoid an invalid TypeIndex landing in the debugger.
      return codeview::TypeIndex::UnsignedCharacter();
    }
    return typeForKindParts(f.byte_size != 0 ? f.byte_size : 4u, f.prim_kind);
  };

  for (const auto &a : inputs_.aggregates) {
    // Phase F+ Sets: Pascal `set of TColor` becomes an LF_STRUCTURE
    // of LF_BITFIELD members, one bit per enumerator. The bit's
    // BitOffset = enumerator ordinal, BitSize = 1. cdb / VS show
    // each enumerator's state (`clRed: 0y1, clBlue: 0y1, ...`)
    // instead of an opaque hex byte. Set width matches Delphi's
    // power-of-two rounding (1/2/4/8 bytes), so the underlying
    // integer width is taken from a.byte_size.
    if (a.kind == AggregateKind::Set) {
      const codeview::TypeIndex underlying =
          a.byte_size == 1   ? codeview::TypeIndex::UnsignedCharacter()
          : a.byte_size == 2 ? codeview::TypeIndex::UInt16Short()
          : a.byte_size == 4 ? codeview::TypeIndex::UInt32()
                             : codeview::TypeIndex::UInt64();
      codeview::ContinuationRecordBuilder sb;
      sb.begin(codeview::ContinuationRecordKind::FieldList);
      for (const auto &e : a.enumerators) {
        codeview::BitFieldRecord bfr(
            underlying,
            /*BitSize=*/1,
            /*BitOffset=*/static_cast<std::uint8_t>(e.value));
        const auto bf_ti = tpi_table_.writeLeafType(bfr);
        codeview::DataMemberRecord dmr(codeview::MemberAccess::Public, bf_ti,
                                       /*offset=*/0, e.name);
        sb.writeMemberType(dmr);
      }
      const auto set_fl_ti = tpi_table_.insertRecord(sb);
      codeview::ClassRecord sr(
          codeview::TypeRecordKind::Struct,
          static_cast<std::uint16_t>(a.enumerators.size()),
          codeview::ClassOptions::None, set_fl_ti, codeview::TypeIndex::None(),
          codeview::TypeIndex::None(), a.byte_size, a.name, std::string{});
      const auto set_ti = tpi_table_.writeLeafType(sr);
      aggregate_inner_ti_.push_back(set_ti);
      aggregate_var_ti_.push_back(set_ti); // value type
      continue;
    }

    // Phase F: enum types take a different shape -- LF_ENUM
    // wrapping an LF_FIELDLIST of LF_ENUMERATEs (one per
    // enumerator). The underlying integer type is picked from
    // the precomputed byte_size (1 -> UChar, 2 -> UShort,
    // 4 -> UInt32) so cdb / VS know how wide to read the
    // memory slot.
    if (a.kind == AggregateKind::Enum) {
      codeview::ContinuationRecordBuilder ecb;
      ecb.begin(codeview::ContinuationRecordKind::FieldList);
      for (const auto &e : a.enumerators) {
        llvm::APSInt apv(64, /*isUnsigned=*/false);
        apv = e.value;
        codeview::EnumeratorRecord er(codeview::MemberAccess::Public, apv,
                                      e.name);
        ecb.writeMemberType(er);
      }
      const auto enum_fl_ti = tpi_table_.insertRecord(ecb);
      const codeview::TypeIndex underlying =
          a.byte_size == 1   ? codeview::TypeIndex::UnsignedCharacter()
          : a.byte_size == 2 ? codeview::TypeIndex::UInt16Short()
                             : codeview::TypeIndex::UInt32();
      codeview::EnumRecord er_rec(
          static_cast<std::uint16_t>(a.enumerators.size()),
          codeview::ClassOptions::None, enum_fl_ti, a.name, std::string{},
          underlying);
      const auto enum_ti = tpi_table_.writeLeafType(er_rec);
      aggregate_inner_ti_.push_back(enum_ti);
      aggregate_var_ti_.push_back(enum_ti); // value type
      continue;
    }

    const bool is_class = (a.kind == AggregateKind::Class);
    const bool has_methods = is_class && !a.methods.empty();

    // FE.1: classes with methods use the forward-decl + complete
    // CodeView pattern. LF_MFUNCTION needs to reference the class
    // type, but LF_ONEMETHOD (which goes INTO the field-list) needs
    // to reference LF_MFUNCTION. The cycle is broken by emitting an
    // LF_CLASS with ForwardReference flag first, deriving the
    // LF_POINTER + LF_MFUNCTION(s) off it, then building the
    // field-list (with LF_ONEMETHOD entries), then the COMPLETE
    // LF_CLASS. cdb resolves forward refs by NAME at consumption.
    codeview::TypeIndex fwd_inner_ti = codeview::TypeIndex::None();
    codeview::TypeIndex fwd_ptr_ti   = codeview::TypeIndex::None();
    std::vector<codeview::TypeIndex> method_mfunc_ti;
    if (has_methods) {
      // Itanium-style mangled unique name: `.?AVClassName@@` for
      // class, `.?AUStructName@@` for struct. MSVC emits one of
      // these for every user type + sets ClassOptions::HasUniqueName.
      // Without it, VS Watch IntelliSense doesn't index the type
      // (typing `lDog->` gives no completion suggestions). Verified
      // against examples/99_cpp_baseline -- MSVC's TDog LF_CLASS
      // carries `unique name: .?AVTDog@@, options: has unique name`.
      const std::string unique_name = ".?AV" + a.name + "@@";
      codeview::ClassRecord fwd(
          codeview::TypeRecordKind::Class,
          /*member_count=*/0,
          codeview::ClassOptions::ForwardReference
              | codeview::ClassOptions::HasUniqueName,
          /*field_list=*/codeview::TypeIndex::None(),
          /*derivation_list=*/codeview::TypeIndex::None(),
          /*vshape=*/codeview::TypeIndex::None(),
          /*size=*/0,
          a.name,
          unique_name);
      fwd_inner_ti = tpi_table_.writeLeafType(fwd);
      // ThisType for class methods is a plain Near64 pointer with
      // PointerOptions::None. Matches MSVC's emission (verified
      // via cl /Zi baseline at examples/99_cpp_baseline/) -- the
      // `Const` we used in FE.1 was the cause of cppvsdbg's "type
      // qualifiers not compatible with the member function" error,
      // not its fix. MSVC's LF_MFUNCTION points at a const-less
      // this-ptr; cppvsdbg expects exactly that.
      codeview::PointerRecord fwd_pr(
          fwd_inner_ti, codeview::PointerKind::Near64,
          codeview::PointerMode::Pointer, codeview::PointerOptions::None,
          /*size=*/8);
      fwd_ptr_ti = tpi_table_.writeLeafType(fwd_pr);
      // FE.2: one LF_ARGLIST per method (deduplicated only for the
      // empty-args case, since most parameterless methods share
      // it). Param TypeIndices come from AggregateMethod::params,
      // each entry routed through resolveTypeIndex like fields are.
      std::optional<codeview::TypeIndex> empty_arglist_ti;
      method_mfunc_ti.reserve(a.methods.size());
      for (const auto& m : a.methods) {
        codeview::TypeIndex arglist_ti;
        if (m.params.empty()) {
          if (!empty_arglist_ti) {
            std::vector<codeview::TypeIndex> empty_args;
            codeview::ArgListRecord empty_arglist(
                codeview::TypeRecordKind::ArgList, empty_args);
            empty_arglist_ti = tpi_table_.writeLeafType(empty_arglist);
          }
          arglist_ti = *empty_arglist_ti;
        } else {
          std::vector<codeview::TypeIndex> arg_tis;
          arg_tis.reserve(m.params.size());
          for (const auto& p : m.params) {
            arg_tis.push_back(
                resolveTypeIndex(p.byte_size, p.prim_kind,
                                 p.aggregate_index));
          }
          codeview::ArgListRecord arglist(
              codeview::TypeRecordKind::ArgList, arg_tis);
          arglist_ti = tpi_table_.writeLeafType(arglist);
        }
        // Pascal `procedure` (no return) -> T_VOID. `function`
        // gets the Int32 default until RSM 0x23 return-type
        // decode lands (FE.x).
        const codeview::TypeIndex return_ti =
            m.returns_void ? codeview::TypeIndex::Void()
                           : codeview::TypeIndex::Int32();
        codeview::MemberFunctionRecord mfunc(
            /*ReturnType=*/return_ti,
            /*ClassType=*/fwd_inner_ti,
            /*ThisType=*/fwd_ptr_ti,
            /*CallConv=*/codeview::CallingConvention::NearC,
            /*Options=*/codeview::FunctionOptions::None,
            /*ParameterCount=*/static_cast<std::uint16_t>(m.params.size()),
            /*ArgumentList=*/arglist_ti,
            /*ThisPointerAdjustment=*/0);
        const auto ti = tpi_table_.writeLeafType(mfunc);
        method_mfunc_ti.push_back(ti);
        method_type_by_qname_[m.qualified_name] = ti;
      }
    }

    codeview::ContinuationRecordBuilder crb;
    crb.begin(codeview::ContinuationRecordKind::FieldList);
    std::uint16_t member_count = 0;
    // LF_BCLASS for class with explicit base (Phase E
    // inheritance). The base lives at field-list position 0; its
    // TypeIndex must be the LF_CLASS, not the pointer wrapper.
    if (is_class && a.base && *a.base < aggregate_inner_ti_.size()) {
      codeview::BaseClassRecord bcr(codeview::MemberAccess::Public,
                                    aggregate_inner_ti_[*a.base],
                                    /*offset=*/0);
      crb.writeMemberType(bcr);
      ++member_count;
    }
    for (const auto &f : a.fields) {
      codeview::DataMemberRecord dmr(codeview::MemberAccess::Public,
                                     fieldTypeIndex(f), f.byte_offset, f.name);
      crb.writeMemberType(dmr);
      ++member_count;
    }
    if (has_methods) {
      for (std::size_t mi = 0; mi < a.methods.size(); ++mi) {
        codeview::OneMethodRecord omr(
            /*Type=*/method_mfunc_ti[mi],
            /*Access=*/codeview::MemberAccess::Public,
            /*MK=*/codeview::MethodKind::Vanilla,
            /*Options=*/codeview::MethodOptions::None,
            /*VFTableOffset=*/0,
            /*Name=*/a.methods[mi].name);
        crb.writeMemberType(omr);
        ++member_count;
      }
    }
    const auto field_list_ti = tpi_table_.insertRecord(crb);
    // Itanium-style unique name (see note on forward-decl above).
    // `.?AV` prefix for classes, `.?AU` for structs/records --
    // matches what MSVC emits. The trailing `@@` is the empty
    // namespace terminator. This + HasUniqueName flag enables VS
    // Watch IntelliSense to index user types for autocomplete.
    const std::string complete_unique_name =
        std::string(is_class ? ".?AV" : ".?AU") + a.name + "@@";
    codeview::ClassRecord cr(is_class ? codeview::TypeRecordKind::Class
                                      : codeview::TypeRecordKind::Struct,
                             member_count,
                             codeview::ClassOptions::HasUniqueName,
                             field_list_ti, codeview::TypeIndex::None(),
                             codeview::TypeIndex::None(), a.byte_size, a.name,
                             complete_unique_name);
    const auto inner_ti = tpi_table_.writeLeafType(cr);
    aggregate_inner_ti_.push_back(inner_ti);

    if (is_class) {
      // Wrap in an 8-byte Near64 LF_POINTER. Variables of class
      // type are pointers; the pointer is what S_REGREL32 /
      // S_GDATA32 ultimately reference.
      codeview::PointerRecord pr(inner_ti, codeview::PointerKind::Near64,
                                 codeview::PointerMode::Pointer,
                                 codeview::PointerOptions::None,
                                 /*size=*/8);
      aggregate_var_ti_.push_back(tpi_table_.writeLeafType(pr));
    } else {
      aggregate_var_ti_.push_back(inner_ti);
    }
  }
}

} // namespace rsm2pdb::pdb::detail
