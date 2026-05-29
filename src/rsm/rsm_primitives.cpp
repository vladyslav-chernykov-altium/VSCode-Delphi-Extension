#include "rsm/rsm_reader.h"
#include "rsm/rsm_internal.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace rsm2pdb::rsm {

using namespace detail;

// -- Primitive type table scan ------------------------------------------
//
// Anchor on the Boolean record's unique signature:
//     2A 07 'B' 'o' 'o' 'l' 'e' 'a' 'n' A8 00 00
// It always sits within the first few KB of the metadata stream. From
// there, walk forward through 0x2A-tagged records until we hit a
// non-0x2A byte. Each record is terminated by an 0xFF byte; the 2-byte
// type-id follows the 0x9C 0x13 marker somewhere inside.
void Reader::scanPrimitiveTable(const std::string& buf) {
    static constexpr char kBooleanSig[] = {
        '\x2A', '\x07', 'B', 'o', 'o', 'l', 'e', 'a', 'n',
        '\xA8', '\x00', '\x00'
    };
    const std::size_t scanStart = std::min<std::size_t>(0x400, buf.size());
    const std::size_t scanEnd   = std::min<std::size_t>(0x10000, buf.size());
    const std::size_t booleanAt = findBytes(buf, scanStart, scanEnd,
        std::string_view(kBooleanSig, sizeof(kBooleanSig)));
    if (booleanAt == std::string::npos) {
        // No Boolean signature within the first 64 KB -> bail silently.
        // The header is still valid; primitives_ is left empty.
        return;
    }

    constexpr std::size_t kMaxRecords = 64;  // generous upper bound

    // The terminator byte 0xFF also occurs inside record payloads
    // (e.g. Cardinal's body contains `f6 ff 81 02`). To find the true
    // record end, search for the NEXT record's start signature
    // (0x2A <namelen> <ASCII letter>) and treat the byte just before
    // that as the terminator. The last primitive in the table is
    // followed by a non-primitive record (different tag) -- when we
    // can't find a plausible 0x2A continuation, we stop.
    auto looksLikePrimitiveStart = [&](std::size_t i) {
        if (i + 3 > buf.size()) return false;
        if (static_cast<std::uint8_t>(buf[i]) != kRecordTagPrimitive) return false;
        const auto len = static_cast<std::uint8_t>(buf[i + 1]);
        if (len == 0 || len > 32) return false;
        if (i + 2 + len > buf.size()) return false;
        const char c = buf[i + 2];
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };

    std::size_t pos = booleanAt;
    while (primitives_.size() < kMaxRecords && pos < buf.size()) {
        const auto tag = static_cast<std::uint8_t>(buf[pos]);
        if (tag != kRecordTagPrimitive) break;   // end of primitive table
        if (pos + 2 > buf.size()) break;
        const auto nameLen = static_cast<std::uint8_t>(buf[pos + 1]);
        const std::size_t nameStart = pos + 2;
        const std::size_t nameEnd   = nameStart + nameLen;
        if (nameEnd > buf.size() || nameLen == 0) break;

        const std::size_t cap = std::min(buf.size(), pos + 256);
        std::size_t nextRec = std::string::npos;
        for (std::size_t i = nameEnd; i + 3 < cap; ++i) {
            if (static_cast<std::uint8_t>(buf[i])     == kRecordTerminator &&
                looksLikePrimitiveStart(i + 1)) {
                nextRec = i + 1;
                break;
            }
        }
        const std::size_t recEnd = (nextRec != std::string::npos)
            ? nextRec : cap;

        const std::string name(buf.data() + nameStart, nameLen);

        std::uint16_t typeId = 0;
        for (std::size_t i = nameEnd; i + 4 <= recEnd; ++i) {
            if (static_cast<std::uint8_t>(buf[i])     == kTypeIdMarker0 &&
                static_cast<std::uint8_t>(buf[i + 1]) == kTypeIdMarker1) {
                typeId =  static_cast<std::uint8_t>(buf[i + 2])
                       | (static_cast<std::uint8_t>(buf[i + 3]) << 8);
                break;
            }
        }

        if (const auto* desc = lookupPrimitiveDesc(name)) {
            Primitive prim;
            prim.name        = name;
            prim.kind        = desc->kind;
            prim.byte_size   = desc->byte_size;
            prim.raw_type_id = typeId;
            prim.file_offset = pos;
            primitives_.push_back(std::move(prim));
        }
        // Unknown Pascal names inside the type table are silently
        // skipped -- they'll be aggregate types we'll decode later.

        if (nextRec == std::string::npos) break;  // last record reached
        pos = nextRec;
    }
}

} // namespace rsm2pdb::rsm
