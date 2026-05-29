#pragma once

// Helpers shared across the rsm_*.cpp submodules that make up the RSM
// reader. Anything in here is `inline` / `inline constexpr` so it can
// live in multiple TUs without ODR violations. Things specific to ONE
// submodule (e.g. the SubRec struct used only by rsm_procs_vars.cpp)
// stay local to that file's anonymous namespace.

#include "model/model.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace rsm2pdb::rsm::detail {

// ---- Record tag constants --------------------------------------------
// See docs/02-rsm-format-notes.md and rsm-format.txt for the empirical
// origin of each value.

inline constexpr std::uint8_t kRecordTagPrimitive   = 0x2A;
inline constexpr std::uint8_t kRecordTagVariable    = 0x20;
inline constexpr std::uint8_t kRecordTagParam       = 0x21;
inline constexpr std::uint8_t kRecordTagVarParam    = 0x22;   // var / out params
inline constexpr std::uint8_t kRecordTagFunction    = 0x28;
inline constexpr std::uint8_t kFunctionEndMarker    = 0x63;
// Post-name sub-tag of a function record. Top three observed values:
// 0xA0 (~98%), 0xE0 (~1.5%, RTL allocators), 0x80 (~0.4%). 0x20 is
// the modern user-code class-method form; 0x41 is the nested-function
// flavour (Pascal `function inside function`) carrying an implicit
// static-link pointer to the parent's stack frame in rcx.
inline constexpr std::uint8_t kFunctionSubTag0      = 0xA0;
inline constexpr std::uint8_t kFunctionSubTag1      = 0xE0;
inline constexpr std::uint8_t kFunctionSubTag2      = 0x80;
inline constexpr std::uint8_t kFunctionSubTag3      = 0x20;
inline constexpr std::uint8_t kFunctionSubTagNested = 0x41;
inline constexpr std::uint8_t kRecordTerminator     = 0xFF;
inline constexpr std::uint8_t kTypeIdMarker0        = 0x9C;
inline constexpr std::uint8_t kTypeIdMarker1        = 0x13;
inline constexpr std::uint8_t kVarPayloadSubTag0    = 0x66;
inline constexpr std::uint8_t kVarPayloadSubTagNP   = 0x62;  // non-primitive variant
inline constexpr std::uint8_t kVarTrailerMarker0    = 0x9C;
inline constexpr std::uint8_t kVarTrailerMarker1    = 0x09;

// ---- Primitive type table --------------------------------------------
//
// Hardcoded Delphi primitive descriptors. Names match the RSM record's
// Pascal name verbatim. Sizes are Win64-specific (Extended is 8 bytes
// on Win64, not 10 as on x86). String family entries cover Delphi's
// pointer-backed managed strings + raw char pointers; `ShortString`
// is NOT in here -- it's an inline length-prefixed byte array and
// falls back to the size-based byte[N] path.

struct PrimitiveDesc {
    std::string_view     name;
    model::PrimitiveKind kind;
    std::uint16_t        byte_size;
};

inline constexpr std::array<PrimitiveDesc, 36> kPrimitiveTable = {{
    {"Boolean",       model::PrimitiveKind::Bool,    1},
    {"ByteBool",      model::PrimitiveKind::Bool,    1},
    {"WordBool",      model::PrimitiveKind::Bool,    2},
    {"LongBool",      model::PrimitiveKind::Bool,    4},
    {"AnsiChar",      model::PrimitiveKind::Char,    1},
    {"Char",          model::PrimitiveKind::WChar,   2},
    {"WideChar",      model::PrimitiveKind::WChar,   2},
    {"ShortInt",      model::PrimitiveKind::Int8,    1},
    {"SmallInt",      model::PrimitiveKind::Int16,   2},
    {"Integer",       model::PrimitiveKind::Int32,   4},
    {"LongInt",       model::PrimitiveKind::Int32,   4},
    {"Int64",         model::PrimitiveKind::Int64,   8},
    {"NativeInt",     model::PrimitiveKind::Int64,   8},
    {"Comp",          model::PrimitiveKind::Int64,   8},
    {"Byte",          model::PrimitiveKind::UInt8,   1},
    {"Word",          model::PrimitiveKind::UInt16,  2},
    {"Cardinal",      model::PrimitiveKind::UInt32,  4},
    {"LongWord",      model::PrimitiveKind::UInt32,  4},
    {"UInt64",        model::PrimitiveKind::UInt64,  8},
    {"NativeUInt",    model::PrimitiveKind::UInt64,  8},
    {"Single",        model::PrimitiveKind::Float32, 4},
    {"Real",          model::PrimitiveKind::Float64, 8},
    {"Real48",        model::PrimitiveKind::Float64, 6},
    {"Double",        model::PrimitiveKind::Float64, 8},
    {"Extended",      model::PrimitiveKind::Float64, 8},
    {"Extended80",    model::PrimitiveKind::Float80, 10},
    {"Currency",      model::PrimitiveKind::Int64,   8},
    {"Pointer",       model::PrimitiveKind::UInt64,  8},
    {"UnicodeString", model::PrimitiveKind::PWChar,  8},
    {"WideString",    model::PrimitiveKind::PWChar,  8},
    {"AnsiString",    model::PrimitiveKind::PChar,   8},
    {"UTF8String",    model::PrimitiveKind::PChar,   8},
    {"PChar",         model::PrimitiveKind::PWChar,  8},
    {"PWideChar",     model::PrimitiveKind::PWChar,  8},
    {"PAnsiChar",     model::PrimitiveKind::PChar,   8},
    {"string",        model::PrimitiveKind::PWChar,  8},
}};

inline const PrimitiveDesc* lookupPrimitiveDesc(std::string_view name) {
    for (const auto& d : kPrimitiveTable) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

// ---- Byte-level helpers ----------------------------------------------

inline std::uint32_t readU32LE(const char* p) {
    return  static_cast<std::uint8_t>(p[0])
         | (static_cast<std::uint8_t>(p[1]) <<  8)
         | (static_cast<std::uint8_t>(p[2]) << 16)
         | (static_cast<std::uint8_t>(p[3]) << 24);
}

// Returns offset of the byte pattern in [start, end), or
// std::string::npos. Patterns here are short and known-unique within
// the search window.
inline std::size_t findBytes(const std::string& buf,
                             std::size_t start,
                             std::size_t end,
                             std::string_view pat) {
    if (pat.empty() || end > buf.size()) return std::string::npos;
    if (end < pat.size()) return std::string::npos;
    const char* base = buf.data();
    for (std::size_t i = start; i + pat.size() <= end; ++i) {
        if (std::memcmp(base + i, pat.data(), pat.size()) == 0) return i;
    }
    return std::string::npos;
}

// True when [p, p+n) is a plausible Pascal identifier name: alnum,
// `_`, `.` (qualified names), `$` / `@` (compiler-internal / RTL
// helpers like System.@Abs). Used by both the variable / proc record
// scanners and the aggregate-types parser to filter out random byte
// sequences that happen to start with a record tag.
inline bool isPrintableName(const char* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = static_cast<unsigned char>(p[i]);
        if (!(std::isalnum(c) || c == '_' || c == '.'
              || c == '$' || c == '@'))
            return false;
    }
    return true;
}

} // namespace rsm2pdb::rsm::detail
