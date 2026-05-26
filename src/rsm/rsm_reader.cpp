#include "rsm/rsm_reader.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace rsm2pdb::rsm {

namespace {

constexpr std::uint8_t kRecordTagPrimitive  = 0x2A;
constexpr std::uint8_t kRecordTagVariable   = 0x20;
constexpr std::uint8_t kRecordTagParam      = 0x21;
constexpr std::uint8_t kRecordTagFunction   = 0x28;
constexpr std::uint8_t kFunctionEndMarker   = 0x63;
constexpr std::uint8_t kFunctionSubTag0     = 0xA0;   // vs vars' 0x66
constexpr std::uint8_t kRecordTerminator    = 0xFF;
constexpr std::uint8_t kTypeIdMarker0       = 0x9C;
constexpr std::uint8_t kTypeIdMarker1       = 0x13;
constexpr std::uint8_t kVarPayloadSubTag0   = 0x66;
constexpr std::uint8_t kVarPayloadSubTagNP  = 0x62;   // non-primitive variant
constexpr std::uint8_t kVarTrailerMarker0   = 0x9C;
constexpr std::uint8_t kVarTrailerMarker1   = 0x09;

std::uint32_t readU32LE(const char* p) {
    return  static_cast<std::uint8_t>(p[0])
         | (static_cast<std::uint8_t>(p[1]) <<  8)
         | (static_cast<std::uint8_t>(p[2]) << 16)
         | (static_cast<std::uint8_t>(p[3]) << 24);
}

// Hardcoded Delphi primitive descriptors. Names match the RSM record's
// Pascal name verbatim. Sizes are Win64-specific (Extended is 8 bytes
// on Win64, not 10 as on x86).
struct PrimitiveDesc {
    std::string_view     name;
    model::PrimitiveKind kind;
    std::uint16_t        byte_size;
};

constexpr std::array<PrimitiveDesc, 28> kPrimitiveTable = {{
    {"Boolean",    model::PrimitiveKind::Bool,    1},
    {"ByteBool",   model::PrimitiveKind::Bool,    1},
    {"WordBool",   model::PrimitiveKind::Bool,    2},
    {"LongBool",   model::PrimitiveKind::Bool,    4},
    {"AnsiChar",   model::PrimitiveKind::Char,    1},
    {"Char",       model::PrimitiveKind::WChar,   2},
    {"WideChar",   model::PrimitiveKind::WChar,   2},
    {"ShortInt",   model::PrimitiveKind::Int8,    1},
    {"SmallInt",   model::PrimitiveKind::Int16,   2},
    {"Integer",    model::PrimitiveKind::Int32,   4},
    {"LongInt",    model::PrimitiveKind::Int32,   4},
    {"Int64",      model::PrimitiveKind::Int64,   8},
    {"NativeInt",  model::PrimitiveKind::Int64,   8},
    {"Comp",       model::PrimitiveKind::Int64,   8},
    {"Byte",       model::PrimitiveKind::UInt8,   1},
    {"Word",       model::PrimitiveKind::UInt16,  2},
    {"Cardinal",   model::PrimitiveKind::UInt32,  4},
    {"LongWord",   model::PrimitiveKind::UInt32,  4},
    {"UInt64",     model::PrimitiveKind::UInt64,  8},
    {"NativeUInt", model::PrimitiveKind::UInt64,  8},
    {"Single",     model::PrimitiveKind::Float32, 4},
    {"Real",       model::PrimitiveKind::Float64, 8},
    {"Real48",     model::PrimitiveKind::Float64, 6},
    {"Double",     model::PrimitiveKind::Float64, 8},
    {"Extended",   model::PrimitiveKind::Float64, 8},
    {"Extended80", model::PrimitiveKind::Float80, 10},
    {"Currency",   model::PrimitiveKind::Int64,   8},
    {"Pointer",    model::PrimitiveKind::UInt64,  8},
}};

const PrimitiveDesc* lookupPrimitiveDesc(std::string_view name) {
    for (const auto& d : kPrimitiveTable) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

// Returns offset of the byte pattern, or std::string::npos. Patterns
// here are short and known-unique within the search window.
std::size_t findBytes(const std::string& buf,
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

} // namespace

bool Reader::open(const std::string& path) {
    path_ = path;
    error_.clear();
    header_ = {};
    primitives_.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error_ = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string buf = ss.str();

    if (buf.size() < 0x21) {
        error_ = "file too short to contain RSM header";
        return false;
    }

    const char* p = buf.data();
    header_.magic = readU32LE(p + 0x00);
    if (header_.magic != Header::kMagic) {
        char m[5] = {p[0], p[1], p[2], p[3], 0};
        error_ = "bad magic: expected \"CSH7\", got \"" + std::string(m) + "\"";
        return false;
    }
    header_.metadata_start   = readU32LE(p + 0x04);
    header_.unit_count       = readU32LE(p + 0x08);
    header_.version_minor    = readU32LE(p + 0x0C);
    header_.timestamp        = readU32LE(p + 0x10);
    header_.flags            = readU32LE(p + 0x14);
    header_.legacy_imagebase = readU32LE(p + 0x18);
    header_.reserved_1c      = readU32LE(p + 0x1C);

    const std::size_t maxScan = buf.size() - 0x20;
    const std::size_t pathLen = ::strnlen(p + 0x20, maxScan);
    header_.exe_path.assign(p + 0x20, pathLen);

    // ---- Primitive type table scan ---------------------------------------
    //
    // Anchor on the Boolean record's unique signature:
    //     2A 07 'B' 'o' 'o' 'l' 'e' 'a' 'n' A8 00 00
    // It always sits within the first few KB of the metadata stream.
    // From there, walk forward through 0x2A-tagged records until we hit
    // a non-0x2A byte. Each record is terminated by an 0xFF byte; the
    // 2-byte type-id follows the 0x9C 0x13 marker somewhere inside.
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
        return true;
    }

    std::size_t pos = booleanAt;
    constexpr std::size_t kMaxRecords = 64;  // generous upper bound

    // The terminator byte 0xFF also occurs inside record payloads
    // (e.g. Cardinal's body contains `f6 ff 81 02`). To find the true
    // record end, search for the NEXT record's start signature
    // (0x2A <namelen> <ASCII letter>) and treat the byte just before
    // that as the terminator. The last primitive in the table is
    // followed by a non-primitive record (different tag) — when we
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

    while (primitives_.size() < kMaxRecords && pos < buf.size()) {
        const auto tag = static_cast<std::uint8_t>(buf[pos]);
        if (tag != kRecordTagPrimitive) break;   // end of primitive table
        if (pos + 2 > buf.size()) break;
        const auto nameLen = static_cast<std::uint8_t>(buf[pos + 1]);
        const std::size_t nameStart = pos + 2;
        const std::size_t nameEnd   = nameStart + nameLen;
        if (nameEnd > buf.size() || nameLen == 0) break;

        // Locate the next primitive record's start to delimit this one.
        const std::size_t cap = std::min(buf.size(), pos + 256);
        std::size_t nextRec = std::string::npos;
        for (std::size_t i = nameEnd; i + 3 < cap; ++i) {
            if (static_cast<std::uint8_t>(buf[i])     == kRecordTerminator &&
                looksLikePrimitiveStart(i + 1)) {
                nextRec = i + 1;
                break;
            }
        }
        // recEnd is exclusive (one past the FF terminator). When this
        // is the last primitive, fall back to a generous scan window
        // so the 9C 13 lookup still works.
        const std::size_t recEnd = (nextRec != std::string::npos)
            ? nextRec : cap;

        const std::string name(buf.data() + nameStart, nameLen);

        // Extract the 2-byte type-id from the 9C 13 marker inside the
        // record body, if present.
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
        // skipped — they'll be aggregate types we'll decode later.

        if (nextRec == std::string::npos) break;  // last record reached
        pos = nextRec;
    }

    // ---- Variable record scan --------------------------------------------
    //
    // Variable records (kind tag 0x20) live in the user-code region
    // near the end of the file. We scan the whole metadata stream
    // looking for the full signature
    //     0x20 <namelen 1..64> <printable-name> 0x66 0x00 0x00
    // and then decode the payload immediately after. Random data
    // hits on the bare 0x20 byte are very common, so we anchor on
    // the full 7+ byte signature plus a printable-ASCII name to
    // keep the false-positive rate negligible.
    //
    // Tag 0x20 ALSO denotes local-variable sub-records inside function
    // records. To avoid double-counting them as globals, we skip any
    // offsets that fall inside a procedure record's body (procedures_
    // is populated by the scan below, which we run FIRST -- see the
    // re-entry note at the bottom of open()). Until that runs, this
    // skip list is empty and the var scan picks up everything.
    auto isPrintableName = [](const char* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const auto c = static_cast<unsigned char>(p[i]);
            // Allow Pascal identifier chars (incl. dot for qualified
            // names, dollar/underscore for compiler-internal ones).
            if (!(std::isalnum(c) || c == '_' || c == '.' || c == '$'))
                return false;
        }
        return true;
    };

    // Step 1: scan procedures FIRST so we can skip their inner bytes.
    // (Body code lives below the variable-scan loop; we duplicate the
    //  proc-scan logic here to keep flow linear.)
    auto scanProcedures = [&]() {
        const std::size_t pscanStart = std::min<std::size_t>(0x426, buf.size());
        std::size_t pi = pscanStart;
        while (pi + 16 < buf.size()) {
            if (static_cast<std::uint8_t>(buf[pi]) != kRecordTagFunction) {
                ++pi;
                continue;
            }
            const auto fnameLen = static_cast<std::uint8_t>(buf[pi + 1]);
            if (fnameLen == 0 || fnameLen > 64) { ++pi; continue; }
            const std::size_t fnameStart = pi + 2;
            const std::size_t headEnd    = fnameStart + fnameLen;
            if (headEnd + 14 >= buf.size()) { ++pi; continue; }
            if (static_cast<std::uint8_t>(buf[headEnd])     != kFunctionSubTag0 ||
                static_cast<std::uint8_t>(buf[headEnd + 1]) != 0x00 ||
                static_cast<std::uint8_t>(buf[headEnd + 2]) != 0x00) {
                ++pi;
                continue;
            }
            if (!isPrintableName(buf.data() + fnameStart, fnameLen)) {
                ++pi;
                continue;
            }

            const std::uint32_t shifted = readU32LE(buf.data() + headEnd + 7);
            const std::uint64_t va = static_cast<std::uint64_t>(shifted) >> 4;

            ProcedureRecord proc;
            proc.name        = std::string(buf.data() + fnameStart, fnameLen);
            proc.address     = va;
            proc.file_offset = pi;

            std::size_t s = headEnd + 16;
            bool ok = true;
            while (s < buf.size()) {
                const auto tag = static_cast<std::uint8_t>(buf[s]);
                if (tag == kFunctionEndMarker) { ++s; break; }
                if (tag != kRecordTagParam && tag != kRecordTagVariable) {
                    ok = false;
                    break;
                }
                if (s + 4 >= buf.size()) { ok = false; break; }
                const auto nlen = static_cast<std::uint8_t>(buf[s + 1]);
                if (nlen == 0 || nlen > 32) { ok = false; break; }
                if (s + 2 + nlen + 5 >= buf.size()) { ok = false; break; }

                const std::size_t bodyAt = s + 2 + nlen;
                const auto t0 = static_cast<std::uint8_t>(buf[bodyAt]);
                if (t0 != kVarPayloadSubTag0 && t0 != kVarPayloadSubTagNP) {
                    ok = false; break;
                }
                if (static_cast<std::uint8_t>(buf[bodyAt + 1]) != 0 ||
                    static_cast<std::uint8_t>(buf[bodyAt + 2]) != 0) {
                    ok = false; break;
                }
                const auto marker = static_cast<std::uint8_t>(buf[bodyAt + 3]);
                const auto offRaw = static_cast<std::int8_t>(buf[bodyAt + 4]);

                Variable sv{};
                sv.name           = std::string(buf.data() + s + 2, nlen);
                sv.address        = 0;
                sv.stack_offset   = static_cast<std::int32_t>(offRaw);
                sv.type_marker    = marker;
                sv.is_primitive   = (t0 == kVarPayloadSubTag0);
                sv.has_trailer    = false;
                sv.inline_type_id = sv.is_primitive ? 0 : marker;
                sv.trailer_type_id = 0;
                sv.file_offset    = s;

                if (tag == kRecordTagParam) {
                    proc.params.push_back(std::move(sv));
                } else {
                    proc.locals.push_back(std::move(sv));
                }
                s = bodyAt + 5;
            }

            if (ok) {
                proc.file_offset_end = s;
                procedures_.push_back(std::move(proc));
                pi = s;
            } else {
                ++pi;
            }
        }
    };
    scanProcedures();

    // Quick predicate: does the given file offset fall inside any
    // procedure record we just parsed?  procedures_ is in increasing
    // file_offset order by construction.
    auto isInsideProcedure = [&](std::size_t off) {
        for (const auto& p : procedures_) {
            if (off >= p.file_offset && off < p.file_offset_end)
                return true;
            if (p.file_offset > off) break;
        }
        return false;
    };

    const std::size_t vscanStart = std::min<std::size_t>(0x426, buf.size());
    std::size_t i = vscanStart;
    while (i + 8 < buf.size()) {
        if (static_cast<std::uint8_t>(buf[i]) != kRecordTagVariable) {
            ++i;
            continue;
        }
        if (isInsideProcedure(i)) { ++i; continue; }
        const auto nameLen = static_cast<std::uint8_t>(buf[i + 1]);
        if (nameLen == 0 || nameLen > 64) { ++i; continue; }
        const std::size_t nameStart = i + 2;
        const std::size_t subTagAt  = nameStart + nameLen;
        if (subTagAt + 3 >= buf.size()) { ++i; continue; }
        if (static_cast<std::uint8_t>(buf[subTagAt])     != kVarPayloadSubTag0 ||
            static_cast<std::uint8_t>(buf[subTagAt + 1]) != 0x00 ||
            static_cast<std::uint8_t>(buf[subTagAt + 2]) != 0x00) {
            ++i;
            continue;
        }
        if (!isPrintableName(buf.data() + nameStart, nameLen)) {
            ++i;
            continue;
        }

        // Decode the payload. Variants:
        //   primitive plain    (5  bytes): <marker u8>  <shifted_va u32>
        //   primitive extended (12 bytes): <marker u8>  <shifted_va u32>
        //                                  0x9C 0x09 <hash u16> <trailer_type_id u16> 0xFF
        //   non-primitive      (6  bytes): <inline_type_id u16-LE> <shifted_va u32>
        //
        // Discriminate by looking at payload[1] (second byte). For the
        // primitive variants the high byte of `shifted_va` lies there
        // and is always small (the legacy ImageBase is 0x004XXXXX, so
        // when shifted left by 4 the top byte of stored_u32 is 0x04..
        // 0x05). For the non-primitive variant payload[1] is the high
        // byte of the type_id (typically 0x06.. or higher). This isn't
        // a foolproof discriminator on its own, but combined with the
        // primitive-form marker being small even (0x02, 0x04, ...) and
        // < 0x20, it's reliable on real Delphi RSMs.
        const std::size_t payloadAt = subTagAt + 3;
        if (payloadAt + 6 > buf.size()) { ++i; continue; }
        Variable v;
        v.name        = std::string(buf.data() + nameStart, nameLen);
        v.file_offset = i;

        const auto b0 = static_cast<std::uint8_t>(buf[payloadAt]);
        const bool primitiveShape = (b0 != 0 && b0 < 0x40 && (b0 & 0x01) == 0);
        if (primitiveShape) {
            // Primitive form (5 or 12 bytes). The extended form has the
            // 0x9C 0x09 trailer marker at payload[5..6].
            const std::uint32_t shifted = readU32LE(buf.data() + payloadAt + 1);
            v.type_marker = b0;
            v.address     = static_cast<std::uint64_t>(shifted) >> 4;
            v.stack_offset = 0;
            v.is_primitive = true;
            v.inline_type_id = 0;

            const bool hasTrailer =
                payloadAt + 12 <= buf.size() &&
                static_cast<std::uint8_t>(buf[payloadAt + 5]) == kVarTrailerMarker0 &&
                static_cast<std::uint8_t>(buf[payloadAt + 6]) == kVarTrailerMarker1 &&
                static_cast<std::uint8_t>(buf[payloadAt + 11]) == kRecordTerminator;
            if (hasTrailer) {
                v.has_trailer = true;
                v.trailer_type_id =
                      static_cast<std::uint8_t>(buf[payloadAt + 9])
                    | (static_cast<std::uint8_t>(buf[payloadAt + 10]) << 8);
                i = payloadAt + 12;
            } else {
                v.has_trailer = false;
                v.trailer_type_id = 0;
                i = payloadAt + 5;
            }
            variables_.push_back(std::move(v));
        } else {
            // Non-primitive form: <type_id u16-LE> <shifted_va u32>
            v.inline_type_id =  static_cast<std::uint8_t>(buf[payloadAt])
                              | (static_cast<std::uint8_t>(buf[payloadAt + 1]) << 8);
            const std::uint32_t shifted = readU32LE(buf.data() + payloadAt + 2);
            v.address       = static_cast<std::uint64_t>(shifted) >> 4;
            v.stack_offset  = 0;
            v.is_primitive  = false;
            v.has_trailer   = false;
            v.type_marker   = 0;
            v.trailer_type_id = 0;
            variables_.push_back(std::move(v));
            i = payloadAt + 6;
        }
    }

    // Build VA -> index maps for O(1) lookup. Skipping addresses below
    // the legacy image base catches the bogus VAs that the variable
    // scanner picks up from procedure-internal byte sequences.
    var_by_va_.reserve(variables_.size());
    for (std::size_t k = 0; k < variables_.size(); ++k) {
        const auto& v = variables_[k];
        if (v.address >= 0x00400000) var_by_va_.emplace(v.address, k);
    }
    proc_by_va_.reserve(procedures_.size());
    for (std::size_t k = 0; k < procedures_.size(); ++k) {
        proc_by_va_.emplace(procedures_[k].address, k);
    }

    return true;
}

const Primitive* Reader::findPrimitive(const std::string& name) const {
    for (const auto& p : primitives_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const Variable* Reader::findVariableAt(std::uint64_t address) const {
    auto it = var_by_va_.find(address);
    return it == var_by_va_.end() ? nullptr : &variables_[it->second];
}

const ProcedureRecord* Reader::findProcedureAt(std::uint64_t address) const {
    auto it = proc_by_va_.find(address);
    return it == proc_by_va_.end() ? nullptr : &procedures_[it->second];
}

void Reader::dump(std::FILE* out) const {
    std::fprintf(out, "RSM file: %s\n", path_.c_str());
    std::fprintf(out, "Header:\n");
    std::fprintf(out, "  0x00 magic            = 0x%08x  \"CSH7\"\n",
                 header_.magic);
    std::fprintf(out, "  0x04 metadata_start   = 0x%08x%s\n",
                 header_.metadata_start,
                 header_.metadata_start == Header::kMetadataStart
                     ? "" : "  (unexpected)");
    std::fprintf(out, "  0x08 unit_count       = %u\n",
                 header_.unit_count);
    std::fprintf(out, "  0x0C version_minor    = %u\n",
                 header_.version_minor);
    std::fprintf(out, "  0x10 timestamp        = 0x%08x\n",
                 header_.timestamp);
    std::fprintf(out, "  0x14 flags            = 0x%08x%s\n",
                 header_.flags,
                 header_.flags == Header::kFlagsConst ? "" : "  (unexpected)");
    std::fprintf(out, "  0x18 legacy_imagebase = 0x%08x%s\n",
                 header_.legacy_imagebase,
                 header_.legacy_imagebase == Header::kLegacyImageBase
                     ? "" : "  (unexpected)");
    std::fprintf(out, "  0x1C reserved         = 0x%08x\n",
                 header_.reserved_1c);
    std::fprintf(out, "  0x20 exe_path         = \"%s\"\n",
                 header_.exe_path.c_str());

    std::fprintf(out, "Primitive types: %zu found\n", primitives_.size());
    if (!primitives_.empty()) {
        std::fprintf(out, "  %-12s %-9s %-5s %-7s %s\n",
                     "name", "kind", "bytes", "type_id", "@offset");
        for (const auto& p : primitives_) {
            const char* kindStr = "?";
            switch (p.kind) {
                case model::PrimitiveKind::Bool:    kindStr = "bool";   break;
                case model::PrimitiveKind::Char:    kindStr = "char";   break;
                case model::PrimitiveKind::WChar:   kindStr = "wchar";  break;
                case model::PrimitiveKind::Int8:    kindStr = "i8";     break;
                case model::PrimitiveKind::Int16:   kindStr = "i16";    break;
                case model::PrimitiveKind::Int32:   kindStr = "i32";    break;
                case model::PrimitiveKind::Int64:   kindStr = "i64";    break;
                case model::PrimitiveKind::UInt8:   kindStr = "u8";     break;
                case model::PrimitiveKind::UInt16:  kindStr = "u16";    break;
                case model::PrimitiveKind::UInt32:  kindStr = "u32";    break;
                case model::PrimitiveKind::UInt64:  kindStr = "u64";    break;
                case model::PrimitiveKind::Float32: kindStr = "f32";    break;
                case model::PrimitiveKind::Float64: kindStr = "f64";    break;
                case model::PrimitiveKind::Float80: kindStr = "f80";    break;
            }
            std::fprintf(out, "  %-12s %-9s %5u 0x%04x  0x%llx\n",
                         p.name.c_str(), kindStr, p.byte_size, p.raw_type_id,
                         static_cast<unsigned long long>(p.file_offset));
        }
    }
    // For the dump output, restrict to records whose VA falls in the
    // plausible PE image range. Our scanner picks up a long tail of
    // false positives (RTL data that happens to contain the variable-
    // record byte pattern) — filtering by VA keeps the report scoped
    // to the records the user actually cares about. The full unfiltered
    // vector is still available via variables() for cross-referencing.
    // VA range covering small Delphi projects (a few MB) up through
    // large DLLs like AdvPCB (~100 MB of code). The bogus VAs that the
    // variable scanner picks up from procedure-internal byte sequences
    // typically land in the gigabyte range, well above this cap.
    constexpr std::uint64_t kVaLo = 0x00400000;   // Win64 default image base
    constexpr std::uint64_t kVaHi = 0x40000000;   // 1 GB cap
    std::size_t shown = 0;
    for (const auto& v : variables_) {
        if (v.address >= kVaLo && v.address < kVaHi) ++shown;
    }
    std::fprintf(out, "Variables: %zu found (%zu in plausible VA range)\n",
                 variables_.size(), shown);
    if (shown > 0) {
        std::fprintf(out, "  %-40s %-16s %-9s %-12s @offset\n",
                     "name", "address", "kind", "type-info");
        for (const auto& v : variables_) {
            if (v.address < kVaLo || v.address >= kVaHi) continue;
            char tid[24];
            if (v.is_primitive) {
                if (v.has_trailer) {
                    std::snprintf(tid, sizeof(tid), "m=0x%02x t=0x%04x",
                                  v.type_marker, v.trailer_type_id);
                } else {
                    std::snprintf(tid, sizeof(tid), "m=0x%02x", v.type_marker);
                }
            } else {
                std::snprintf(tid, sizeof(tid), "t=0x%04x", v.inline_type_id);
            }
            std::fprintf(out, "  %-40s 0x%014llx %-9s %-12s @0x%llx\n",
                         v.name.c_str(),
                         static_cast<unsigned long long>(v.address),
                         v.is_primitive ? "primitive" : "byref",
                         tid,
                         static_cast<unsigned long long>(v.file_offset));
        }
    }
    // ---- Procedures ---------------------------------------------------
    std::size_t pshown = 0;
    for (const auto& p : procedures_) {
        if (p.address >= kVaLo && p.address < kVaHi) ++pshown;
    }
    std::fprintf(out, "Procedures: %zu found (%zu in plausible VA range)\n",
                 procedures_.size(), pshown);
    if (pshown > 0) {
        std::fprintf(out, "  %-32s %-16s %-6s %-6s @offset\n",
                     "name", "address", "params", "locals");
        for (const auto& p : procedures_) {
            if (p.address < kVaLo || p.address >= kVaHi) continue;
            std::fprintf(out, "  %-32s 0x%014llx %-6zu %-6zu @0x%llx\n",
                         p.name.c_str(),
                         static_cast<unsigned long long>(p.address),
                         p.params.size(), p.locals.size(),
                         static_cast<unsigned long long>(p.file_offset));
            for (const auto& v : p.params) {
                std::fprintf(out, "    param %-20s marker=0x%02x off=%+d %s\n",
                             v.name.c_str(), v.type_marker,
                             v.stack_offset,
                             v.is_primitive ? "(primitive)" : "(byref)");
            }
            for (const auto& v : p.locals) {
                std::fprintf(out, "    local %-20s marker=0x%02x off=%+d %s\n",
                             v.name.c_str(), v.type_marker,
                             v.stack_offset,
                             v.is_primitive ? "(primitive)" : "(byref)");
            }
        }
    }

    std::fprintf(out,
                 "(metadata stream at 0x%08x: header + primitives + variables + procedures parsed)\n",
                 header_.metadata_start);
}

// -------------------------------------------------------------------------
// Type decoration: RSM -> model::Module
// -------------------------------------------------------------------------

namespace {

// Heuristic mapping from byte-size to the most common Pascal
// primitive of that size. Loses Cardinal/Single (4 bytes) and
// UInt64/Double (8 bytes) distinction but covers the >95% case.
// Returns std::nullopt for non-standard sizes; the caller falls
// back to a `byte[N]` array so the displayed width matches the
// actual memory footprint.
std::optional<model::PrimitiveKind> primitiveKindForSize(std::uint64_t size) {
    switch (size) {
    case 1: return model::PrimitiveKind::UInt8;   // Byte
    case 2: return model::PrimitiveKind::UInt16;  // Word
    case 4: return model::PrimitiveKind::Int32;   // Integer
    case 8: return model::PrimitiveKind::Int64;   // Int64
    default: return std::nullopt;
    }
}

} // namespace

void decorateTypes(const Reader& reader, model::Module& mod) {
    // Collect all variable VAs across the whole module so we can
    // compute next-symbol-gap as a size estimate for each variable.
    std::vector<std::uint64_t> all_var_vas;
    all_var_vas.reserve(64);
    for (const auto& cu : mod.units) {
        for (const auto& s : cu.symbols) {
            if (s.kind == model::SymbolKind::Variable) {
                all_var_vas.push_back(s.address);
            }
        }
    }
    std::sort(all_var_vas.begin(), all_var_vas.end());
    all_var_vas.erase(std::unique(all_var_vas.begin(), all_var_vas.end()),
                      all_var_vas.end());

    auto nextHigher = [&](std::uint64_t va) -> std::uint64_t {
        auto it = std::upper_bound(all_var_vas.begin(), all_var_vas.end(), va);
        return it == all_var_vas.end() ? 0 : *it;
    };

    // Memoize "byte[N]" array types so identical fallbacks share a TypeId.
    // We need a single underlying `byte` base type; create it on first use.
    model::TypeId byte_type_id = model::kNoType;
    auto byteType = [&]() {
        if (byte_type_id == model::kNoType) {
            byte_type_id = mod.addType(
                model::Type{model::PrimitiveType{model::PrimitiveKind::UInt8}});
        }
        return byte_type_id;
    };
    std::map<std::uint64_t, model::TypeId> byte_array_cache;
    auto byteArrayOfSize = [&](std::uint64_t n) {
        auto it = byte_array_cache.find(n);
        if (it != byte_array_cache.end()) return it->second;
        model::TypeId tid = mod.addType(
            model::Type{model::ArrayType{byteType(), n}});
        byte_array_cache[n] = tid;
        return tid;
    };

    // Process each compile unit in turn. Markers are unit-local, so
    // the marker-grouping happens per CU.
    for (auto& cu : mod.units) {
        // Group primitive variables by their type_marker; collect
        // non-primitive variables separately.
        std::map<std::uint8_t, std::vector<model::Symbol*>> by_marker;
        std::vector<model::Symbol*> non_primitives;

        for (auto& s : cu.symbols) {
            if (s.kind != model::SymbolKind::Variable) continue;
            const auto* v = reader.findVariableAt(s.address);
            if (!v) continue;
            if (v->is_primitive) {
                by_marker[v->type_marker].push_back(&s);
            } else {
                non_primitives.push_back(&s);
            }
        }

        // Primitive marker groups: one PrimitiveType per marker.
        // For non-standard sizes (when alignment padding inflates the
        // gap past a primitive width) fall back to a byte[N] array.
        for (auto& [marker, syms] : by_marker) {
            std::uint64_t min_gap = 0;
            for (auto* s : syms) {
                const auto next = nextHigher(s->address);
                if (next > s->address) {
                    const auto gap = next - s->address;
                    if (min_gap == 0 || gap < min_gap) min_gap = gap;
                }
            }
            if (min_gap == 0) min_gap = 4;  // last symbol; assume Integer

            model::TypeId tid;
            if (auto kind = primitiveKindForSize(min_gap)) {
                tid = mod.addType(model::Type{model::PrimitiveType{*kind}});
            } else {
                tid = byteArrayOfSize(min_gap);
            }
            for (auto* s : syms) s->type = tid;
        }

        // Non-primitive variables: byte[N] fallback sized by next-gap.
        for (auto* s : non_primitives) {
            const auto next = nextHigher(s->address);
            std::uint64_t n = (next > s->address) ? (next - s->address) : 1;
            if (n == 0) n = 1;
            s->type = byteArrayOfSize(n);
        }

        // -- Procedure params + locals -------------------------------------
        //
        // For each function Symbol in this CU, look up its procedure
        // record in the RSM by VA. Then map each param/local's per-unit
        // type_marker -> TypeId, reusing the same marker->type table we
        // built for globals where available. For markers not seen among
        // globals (e.g. Geometry has only procedures, no globals at all)
        // fall back to Integer (4-byte signed) for primitives and
        // byte[8] for non-primitives.
        //
        // Build a marker->TypeId map from the just-processed globals.
        std::map<std::uint8_t, model::TypeId> marker_to_type;
        for (const auto& [marker, syms] : by_marker) {
            if (!syms.empty()) marker_to_type[marker] = syms.front()->type;
        }
        model::TypeId default_primitive_id = model::kNoType;
        auto integerType = [&]() {
            if (default_primitive_id == model::kNoType) {
                default_primitive_id = mod.addType(
                    model::Type{model::PrimitiveType{model::PrimitiveKind::Int32}});
            }
            return default_primitive_id;
        };

        auto resolveParamLocalType = [&](const Variable& v) -> model::TypeId {
            if (v.is_primitive) {
                auto it = marker_to_type.find(v.type_marker);
                if (it != marker_to_type.end()) return it->second;
                return integerType();   // unit has no global of this marker
            } else {
                // Records / classes / enums: byte[N] fallback. We don't
                // know N for params/locals at this stage -- default to
                // a sensible 8 bytes which fits TPoint and most small
                // records. Refine later when we decode aggregate types.
                return byteArrayOfSize(8);
            }
        };

        for (auto& s : cu.symbols) {
            if (s.kind != model::SymbolKind::Function) continue;
            const auto* proc = reader.findProcedureAt(s.address);
            if (!proc) continue;

            s.params.reserve(proc->params.size());
            for (const auto& pv : proc->params) {
                model::LocalVar lv;
                lv.name         = pv.name;
                lv.type         = resolveParamLocalType(pv);
                lv.stack_offset = pv.stack_offset;
                s.params.push_back(std::move(lv));
            }
            s.locals.reserve(proc->locals.size());
            for (const auto& lvar : proc->locals) {
                model::LocalVar lv;
                lv.name         = lvar.name;
                lv.type         = resolveParamLocalType(lvar);
                lv.stack_offset = lvar.stack_offset;
                s.locals.push_back(std::move(lv));
            }
        }
    }
}

} // namespace rsm2pdb::rsm
