// DWARF v5 emitter entry point. Emits one .debug_info CU per
// CompileUnit; .debug_abbrev / .debug_line writers live in
// dwarf_sections.cpp. Helpers (BytesBuf, abbrev codes,
// AddressRange) in dwarf_internal.h.

#include "dwarf/dwarf_emitter.h"
#include "dwarf/dwarf_internal.h"

#include "llvm/BinaryFormat/Dwarf.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace rsm2pdb::dwarf {

namespace {

using detail::BytesBuf;
using detail::AddressRange;
using detail::kAbbrevCu;
using detail::kAbbrevSubprogram;
using detail::kAbbrevVariable;
using detail::kAbbrevTypedVariable;
using detail::kAbbrevBaseType;
using detail::kAbbrevArrayType;
using detail::kAbbrevSubrangeType;
using detail::kAbbrevSubprogramWithKids;
using detail::kAbbrevFormalParameter;
using detail::kAbbrevLocalVariable;

AddressRange cuRange(const model::CompileUnit& cu) {
    AddressRange r{0, 0};
    bool first = true;
    auto absorb = [&](std::uint64_t a) {
        if (a == 0) return;
        if (first) { r.lo = r.hi = a; first = false; }
        else {
            if (a < r.lo) r.lo = a;
            if (a > r.hi) r.hi = a;
        }
    };
    for (const auto& le : cu.lines) absorb(le.address);
    for (const auto& sym : cu.symbols) {
        if (sym.kind == model::SymbolKind::Function) absorb(sym.address);
    }
    return r;
}

// ---------------------------------------------------------------------
// Compute high_pc for a symbol: use the next-higher symbol's address
// within the same CU, falling back to the CU's high_pc.
// ---------------------------------------------------------------------
std::uint64_t symbolEnd(const model::CompileUnit& cu,
                        const model::Symbol& sym,
                        std::uint64_t cu_high) {
    std::uint64_t best = cu_high;
    for (const auto& other : cu.symbols) {
        if (other.address > sym.address && other.address < best) {
            best = other.address;
        }
    }
    return best > sym.address ? best : sym.address + 1;
}

// ---------------------------------------------------------------------
// Pascal-friendly display name for a PrimitiveKind. Pairs with the
// DWARF encoding so debuggers render values correctly.
// ---------------------------------------------------------------------
struct PrimitiveDescriptor {
    const char*   name;
    std::uint8_t  byte_size;
    std::uint8_t  encoding;   // llvm::dwarf::DW_ATE_*
};
PrimitiveDescriptor describePrimitive(model::PrimitiveKind k) {
    using namespace llvm::dwarf;
    switch (k) {
    case model::PrimitiveKind::Bool:    return {"Boolean", 1, DW_ATE_boolean};
    case model::PrimitiveKind::Char:    return {"AnsiChar", 1, DW_ATE_unsigned_char};
    case model::PrimitiveKind::WChar:   return {"Char", 2, DW_ATE_UTF};
    case model::PrimitiveKind::Int8:    return {"ShortInt", 1, DW_ATE_signed};
    case model::PrimitiveKind::Int16:   return {"SmallInt", 2, DW_ATE_signed};
    case model::PrimitiveKind::Int32:   return {"Integer",  4, DW_ATE_signed};
    case model::PrimitiveKind::Int64:   return {"Int64",    8, DW_ATE_signed};
    case model::PrimitiveKind::UInt8:   return {"Byte",     1, DW_ATE_unsigned};
    case model::PrimitiveKind::UInt16:  return {"Word",     2, DW_ATE_unsigned};
    case model::PrimitiveKind::UInt32:  return {"Cardinal", 4, DW_ATE_unsigned};
    case model::PrimitiveKind::UInt64:  return {"UInt64",   8, DW_ATE_unsigned};
    case model::PrimitiveKind::Float32: return {"Single",   4, DW_ATE_float};
    case model::PrimitiveKind::Float64: return {"Double",   8, DW_ATE_float};
    case model::PrimitiveKind::Float80: return {"Extended", 10, DW_ATE_float};
    // String-pointer primitives. DWARF has no built-in
    // pointer-to-char "string display" flag, so we describe them as
    // 8-byte unsigned addresses (the gdb pretty-printer can still
    // dereference manually). The PDB backend renders them as native
    // CodeView pointer-to-char simple types where cdb auto-displays
    // them as `"..."`.
    case model::PrimitiveKind::PChar:   return {"PAnsiChar", 8, DW_ATE_address};
    case model::PrimitiveKind::PWChar:  return {"PWideChar", 8, DW_ATE_address};
    }
    return {"unknown", 1, DW_ATE_unsigned};
}

// ---------------------------------------------------------------------
// Emit a single type DIE; returns the absolute byte offset within
// .debug_info where the DIE begins (used for DW_AT_type refs).
// ---------------------------------------------------------------------
std::uint32_t emitBaseTypeDIE(BytesBuf& info, model::PrimitiveKind k) {
    const auto off = static_cast<std::uint32_t>(info.size());
    const auto desc = describePrimitive(k);
    info.uleb128(kAbbrevBaseType);
    info.cstr(desc.name);
    info.u8(desc.byte_size);
    info.u8(desc.encoding);
    return off;
}

std::uint32_t emitArrayTypeDIE(BytesBuf& info,
                               std::uint32_t element_offset,
                               std::uint64_t length) {
    const auto off = static_cast<std::uint32_t>(info.size());
    info.uleb128(kAbbrevArrayType);
    info.u32(element_offset);
    // Single subrange child. gdb's Pascal mode defaults the lower
    // bound to 1, so emit upper_bound = length to cover all N elements
    // as the Pascal range [1..N].
    info.uleb128(kAbbrevSubrangeType);
    info.uleb128(length);                          // DW_AT_upper_bound
    info.u8(0);                                    // children-end marker
    return off;
}

// ---------------------------------------------------------------------
// Compile-unit DIE for .debug_info.

// Walk this CU's symbols, emit unique base-types and array-types up
// front, and record their absolute byte positions in `type_offsets` so
// later DW_AT_type references can point at them. Two-pass: array
// elements are emitted before their containing arrays.
void emitTypeDIEs(BytesBuf& info,
                  const model::Module& mod,
                  const model::CompileUnit& cu,
                  std::map<model::TypeId, std::uint32_t>& type_offsets) {
    auto ensureType = [&](model::TypeId tid) -> std::uint32_t {
        if (tid == model::kNoType) return 0;
        auto it = type_offsets.find(tid);
        if (it != type_offsets.end()) return it->second;
        const auto& t = mod.getType(tid);
        std::uint32_t off = 0;
        if (std::holds_alternative<model::PrimitiveType>(t.kind)) {
            const auto& p = std::get<model::PrimitiveType>(t.kind);
            off = emitBaseTypeDIE(info, p.kind);
        } else if (std::holds_alternative<model::ArrayType>(t.kind)) {
            const auto& a = std::get<model::ArrayType>(t.kind);
            // Element type was emitted in the prepass below.
            std::uint32_t element_off = 0;
            if (a.element != model::kNoType) {
                auto eit = type_offsets.find(a.element);
                if (eit != type_offsets.end()) element_off = eit->second;
            }
            off = emitArrayTypeDIE(info, element_off, a.length);
        } else {
            // RecordType / EnumType / PointerType: not supported in
            // B-lite. Caller falls back to the untyped abbrev.
            return 0;
        }
        type_offsets[tid] = off;
        return off;
    };

    auto preEnsureArrayElement = [&](model::TypeId tid) {
        if (tid == model::kNoType) return;
        const auto& t = mod.getType(tid);
        if (std::holds_alternative<model::ArrayType>(t.kind)) {
            const auto& a = std::get<model::ArrayType>(t.kind);
            if (a.element != model::kNoType) ensureType(a.element);
        }
    };
    for (const auto& sym : cu.symbols) {
        if (sym.kind == model::SymbolKind::Variable) preEnsureArrayElement(sym.type);
        if (sym.kind == model::SymbolKind::Function) {
            for (const auto& p : sym.params) preEnsureArrayElement(p.type);
            for (const auto& l : sym.locals) preEnsureArrayElement(l.type);
        }
    }
    for (const auto& sym : cu.symbols) {
        if (sym.kind == model::SymbolKind::Variable) ensureType(sym.type);
        if (sym.kind == model::SymbolKind::Function) {
            for (const auto& p : sym.params) ensureType(p.type);
            for (const auto& l : sym.locals) ensureType(l.type);
        }
    }
}

// Emit subprogram (function) + data (variable) DIEs for the CU. Function
// symbols must fall within `rng`; variables are unrestricted. Each
// subprogram with params or locals carries DW_AT_frame_base = DW_OP_breg6(0)
// and each var emits DW_OP_fbreg(stack_offset) -- stack_offset is the

void emitSubprogramAndVariableDIEs(
        BytesBuf& info,
        const model::CompileUnit& cu,
        const AddressRange& rng,
        const std::map<model::TypeId, std::uint32_t>& type_offsets) {
    using namespace llvm::dwarf;

    // Serialize a signed integer as SLEB128 into a fresh vector. We need
    // the byte count up front to fill in the exprloc length prefix, so
    // we encode into a temporary then splice.
    auto sleb128Bytes = [](std::int64_t v) {
        std::vector<std::uint8_t> out;
        bool more = true;
        while (more) {
            std::uint8_t b = static_cast<std::uint8_t>(v & 0x7F);
            v >>= 7;
            const bool sign = (b & 0x40) != 0;
            if ((v == 0 && !sign) || (v == -1 && sign)) more = false;
            else b |= 0x80;
            out.push_back(b);
        }
        return out;
    };

    for (const auto& sym : cu.symbols) {
        switch (sym.kind) {
        case model::SymbolKind::Function: {
            if (sym.address < rng.lo || sym.address > rng.hi) break;
            std::uint64_t end = symbolEnd(cu, sym, rng.hi + 1);
            const bool has_kids = !sym.params.empty() || !sym.locals.empty();

            if (!has_kids) {
                info.uleb128(kAbbrevSubprogram);
                info.cstr(sym.name);
                info.u64(sym.address);
                info.u64(end);
                info.u8(1);  // external
            } else {
                info.uleb128(kAbbrevSubprogramWithKids);
                info.cstr(sym.name);
                info.u64(sym.address);
                info.u64(end);
                info.u8(1);                       // external
                // frame_base exprloc: DW_OP_breg6 sleb128(0) -- frame
                // base is RBP itself; each var is `[rbp + stack_offset]`.
                {
                    auto leb = sleb128Bytes(0);
                    info.uleb128(1 + leb.size());
                    info.u8(DW_OP_breg6);
                    info.raw(leb.data(), leb.size());
                }

                auto emitParamOrLocal = [&](const model::LocalVar& v,
                                            std::uint64_t abbrev_code) {
                    std::uint32_t type_off = 0;
                    if (v.type != model::kNoType) {
                        auto it = type_offsets.find(v.type);
                        if (it != type_offsets.end()) type_off = it->second;
                    }
                    info.uleb128(abbrev_code);
                    info.cstr(v.name);
                    info.u32(type_off);
                    auto leb = sleb128Bytes(v.stack_offset);
                    info.uleb128(1 + leb.size());
                    info.u8(DW_OP_fbreg);
                    info.raw(leb.data(), leb.size());
                };

                for (const auto& p : sym.params)
                    emitParamOrLocal(p, kAbbrevFormalParameter);
                for (const auto& l : sym.locals)
                    emitParamOrLocal(l, kAbbrevLocalVariable);

                info.u8(0);                       // children-end marker
            }
            break;
        }
        case model::SymbolKind::Variable: {
            // Choose typed-variable abbrev (4) when we have a resolved
            // type DIE; otherwise fall back to the untyped abbrev (3).
            std::uint32_t type_off = 0;
            if (sym.type != model::kNoType) {
                auto it = type_offsets.find(sym.type);
                if (it != type_offsets.end()) type_off = it->second;
            }
            if (type_off != 0) {
                info.uleb128(kAbbrevTypedVariable);
                info.cstr(sym.name);
                info.uleb128(1 + 8);
                info.u8(DW_OP_addr);
                info.u64(sym.address);
                info.u32(type_off);
                info.u8(1);  // external
            } else {
                info.uleb128(kAbbrevVariable);
                info.cstr(sym.name);
                info.uleb128(1 + 8);
                info.u8(DW_OP_addr);
                info.u64(sym.address);
                info.u8(1);  // external
            }
            break;
        }
        case model::SymbolKind::Unknown:
            // Skip; we don't know how to describe it.
            break;
        }
    }
}

void writeCompileUnit(BytesBuf& info,
                      const model::Module& mod,
                      const model::CompileUnit& cu,
                      const std::string& producer,
                      std::uint32_t stmt_list_offset,
                      std::uint32_t abbrev_table_offset) {
    using namespace llvm::dwarf;

    // CU header (DWARF v5, 32-bit DWARF):
    //   u32 unit_length     (excluding this field)
    //   u16 version         (5)
    //   u8  unit_type       (DW_UT_compile)
    //   u8  address_size    (8)
    //   u32 debug_abbrev_offset
    std::size_t unit_length_pos = info.reservedU32();
    std::size_t length_field_end = info.size();

    info.u16(5);
    info.u8(DW_UT_compile);
    info.u8(8);
    info.u32(abbrev_table_offset);

    // DIE: abbrev code 1 with attributes in order matching writeAbbrevTable.
    info.uleb128(kAbbrevCu);
    info.cstr(producer);                  // DW_AT_producer        (string)
    info.u16(DW_LANG_Pascal83);           // DW_AT_language        (data2)

    auto split = detail::splitPath(cu.source_path);
    info.cstr(split.file);                // DW_AT_name            (string)
    info.cstr(split.dir);                 // DW_AT_comp_dir        (string)

    auto rng = cuRange(cu);
    info.u64(rng.lo);                     // DW_AT_low_pc          (addr)
    info.u64(rng.hi + 1);                 // DW_AT_high_pc         (addr, exclusive)
    info.u32(stmt_list_offset);           // DW_AT_stmt_list       (sec_offset)

    // ---- Type DIEs first, then child DIEs (subprograms + variables) ----
    std::map<model::TypeId, std::uint32_t> type_offsets;
    emitTypeDIEs(info, mod, cu, type_offsets);
    emitSubprogramAndVariableDIEs(info, cu, rng, type_offsets);

    // End-of-children marker for the CU.
    info.u8(0);

    // Patch unit_length.
    std::size_t total_after_length = info.size() - length_field_end;
    info.patchU32(unit_length_pos, static_cast<std::uint32_t>(total_after_length));
}

} // anonymous namespace

// =========================================================================

bool emit(const model::Module& mod,
          const EmitOptions& opts,
          DwarfSections& out,
          std::string& error_out) {
    if (mod.units.empty()) {
        error_out = "model has no compile units";
        return false;
    }

    BytesBuf info, abbrev, line, str, line_str;

    detail::writeAbbrevTable(abbrev);
    const std::uint32_t abbrev_offset = 0;   // single table; CU refers to byte 0

    for (const auto& cu : mod.units) {
        if (cu.lines.empty()) continue;       // skip CUs with no line info
        const std::uint32_t stmt_list_off = detail::writeLineProgram(line, cu);
        writeCompileUnit(info, mod, cu, opts.producer, stmt_list_off, abbrev_offset);
    }

    out.debug_info     = std::move(info).take();
    out.debug_abbrev   = std::move(abbrev).take();
    out.debug_line     = std::move(line).take();
    out.debug_str      = std::move(str).take();     // empty
    out.debug_line_str = std::move(line_str).take(); // empty

    return true;
}

} // namespace rsm2pdb::dwarf
