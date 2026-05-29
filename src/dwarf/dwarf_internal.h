#pragma once

// Internal helpers shared across the dwarf_*.cpp submodules.
// Public clients consume dwarf_emitter.h only.

#include "dwarf/dwarf_emitter.h"
#include "model/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rsm2pdb::dwarf::detail {

// ---------------------------------------------------------------------
// Byte buffer with little-endian writes and LEB128 encoding.
// ---------------------------------------------------------------------
class BytesBuf {
public:
    void u8(std::uint8_t v)  { bytes_.push_back(v); }
    void u16(std::uint16_t v) {
        bytes_.push_back(static_cast<std::uint8_t>(v));
        bytes_.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes_.push_back(static_cast<std::uint8_t>(v >> (i*8)));
    }
    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes_.push_back(static_cast<std::uint8_t>(v >> (i*8)));
    }
    void s8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }

    void uleb128(std::uint64_t v) {
        do {
            std::uint8_t b = v & 0x7F;
            v >>= 7;
            if (v) b |= 0x80;
            bytes_.push_back(b);
        } while (v);
    }
    void sleb128(std::int64_t v) {
        bool more = true;
        while (more) {
            std::uint8_t b = v & 0x7F;
            v >>= 7;  // arithmetic shift
            bool sign = (b & 0x40) != 0;
            if ((v == 0 && !sign) || (v == -1 && sign)) {
                more = false;
            } else {
                b |= 0x80;
            }
            bytes_.push_back(b);
        }
    }

    // Null-terminated string.
    void cstr(const std::string& s) {
        bytes_.insert(bytes_.end(), s.begin(), s.end());
        bytes_.push_back(0);
    }

    void raw(const void* data, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes_.insert(bytes_.end(), p, p + n);
    }

    // Reserve a fixed-size placeholder; returns its offset so the caller
    // can patch later (used for 4-byte unit_length fields).
    std::size_t reservedU32() {
        std::size_t off = bytes_.size();
        bytes_.insert(bytes_.end(), 4, 0);
        return off;
    }
    void patchU32(std::size_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            bytes_[off + i] = static_cast<std::uint8_t>(v >> (i*8));
    }

    std::size_t size() const { return bytes_.size(); }
    std::vector<std::uint8_t>&& take() && { return std::move(bytes_); }
    const std::vector<std::uint8_t>& view() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

// ---------------------------------------------------------------------
// Abbreviation codes shared between writeAbbrevTable (dwarf_sections.cpp)
// and the DIE emitters (dwarf_emitter.cpp).
// ---------------------------------------------------------------------
inline constexpr std::uint64_t kAbbrevCu                 = 1;
inline constexpr std::uint64_t kAbbrevSubprogram         = 2;
inline constexpr std::uint64_t kAbbrevVariable           = 3;
inline constexpr std::uint64_t kAbbrevTypedVariable      = 4;
inline constexpr std::uint64_t kAbbrevBaseType           = 5;
inline constexpr std::uint64_t kAbbrevArrayType          = 6;
inline constexpr std::uint64_t kAbbrevSubrangeType       = 7;
inline constexpr std::uint64_t kAbbrevSubprogramWithKids = 8;
inline constexpr std::uint64_t kAbbrevFormalParameter    = 9;
inline constexpr std::uint64_t kAbbrevLocalVariable      = 10;

// Address range of a CU's code (low_pc / high_pc).
struct AddressRange { std::uint64_t lo, hi; };

// Split a filesystem path into (directory, file). Both relative-friendly.
struct PathSplit { std::string dir, file; };
inline PathSplit splitPath(const std::string& path) {
    auto slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return {"", path};
    return {path.substr(0, slash), path.substr(slash + 1)};
}

// ---------------------------------------------------------------------
// Forward declarations for cross-TU phase functions.
// Bodies live in:
//   writeAbbrevTable / writeLineProgram  -> dwarf_sections.cpp
//   emitTypeDIEs / emitSubprogramAndVariableDIEs / writeCompileUnit
//                                        -> dwarf_emitter.cpp
// ---------------------------------------------------------------------
void writeAbbrevTable(BytesBuf& abbrev);
std::uint32_t writeLineProgram(BytesBuf& line, const model::CompileUnit& cu);

} // namespace rsm2pdb::dwarf::detail
