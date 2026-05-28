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
#include <unordered_set>
#include <vector>

namespace rsm2pdb::rsm {

namespace {

constexpr std::uint8_t kRecordTagPrimitive  = 0x2A;
constexpr std::uint8_t kRecordTagVariable   = 0x20;
constexpr std::uint8_t kRecordTagParam      = 0x21;
constexpr std::uint8_t kRecordTagVarParam   = 0x22;   // var / out params
constexpr std::uint8_t kRecordTagFunction   = 0x28;
constexpr std::uint8_t kFunctionEndMarker   = 0x63;
// Post-name sub-tag of a function record; bit-flag-like, encodes some
// per-function attribute we don't decode (likely virtual/static/inline
// or calling convention). Empirically the top three values are 0xA0
// (~98%), 0xE0 (~1.5%, RTL allocators), 0x80 (~0.4%). Other values
// (~0.06%) appear to be byte-level false positives.
constexpr std::uint8_t kFunctionSubTag0     = 0xA0;   // vs vars' 0x66
constexpr std::uint8_t kFunctionSubTag1     = 0xE0;
constexpr std::uint8_t kFunctionSubTag2     = 0x80;
constexpr std::uint8_t kFunctionSubTag3     = 0x20;   // user-code methods
constexpr std::uint8_t kFunctionSubTagNested = 0x41;  // nested functions
                                                      // (Pascal `function
                                                      // inside function`):
                                                      // takes an implicit
                                                      // static-link pointer
                                                      // to the parent's
                                                      // stack frame in rcx.
                                                       // (Delphi 10.x emits
                                                       //  this for TClass.X)
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

constexpr std::array<PrimitiveDesc, 36> kPrimitiveTable = {{
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
    // String family. Delphi's modern `string`/`UnicodeString` and
    // `WideString` are 8-byte pointers to UTF-16 char sequences (with
    // a hidden RTL header just below); `AnsiString`/`UTF8String` are
    // pointers to 1-byte char sequences. `PChar`/`PAnsiChar`/
    // `PWideChar` are the raw pointer aliases (PChar = PWideChar on
    // modern Delphi). `ShortString` is NOT pointer-based (it's an
    // inline length-prefixed byte array, default 256 bytes) so we
    // leave it out -- the size-based fallback emits it as byte[N].
    {"UnicodeString", model::PrimitiveKind::PWChar,  8},
    {"WideString",    model::PrimitiveKind::PWChar,  8},
    {"AnsiString",    model::PrimitiveKind::PChar,   8},
    {"UTF8String",    model::PrimitiveKind::PChar,   8},
    {"PChar",         model::PrimitiveKind::PWChar,  8},
    {"PWideChar",     model::PrimitiveKind::PWChar,  8},
    {"PAnsiChar",     model::PrimitiveKind::PChar,   8},
    {"string",        model::PrimitiveKind::PWChar,  8},
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
            // Pascal identifier chars (incl. '.' for qualified names,
            // '$' / '_' for compiler-internal ones, '@' for RTL helpers
            // like System.@Abs).
            if (!(std::isalnum(c) || c == '_' || c == '.'
                  || c == '$' || c == '@'))
                return false;
        }
        return true;
    };

    // Step 1: scan procedures FIRST so we can skip their inner bytes.
    //
    // Format (validated against real-world RSMs, see docs/02-rsm-format-notes.md
    // and the empirical histograms produced by `rsm2pdb analyze-procs`):
    //
    //   0x28 <namelen u8> <name>
    //   <sub-tag {0xA0,0xE0,0x80}> 0x00 0x00
    //   <hash u32> <shifted_va u32>
    //   <variable-length trailer 3..30 bytes; not decoded yet>
    //   sub-records ... (zero or more)
    //   0x63                                            -- record-end marker
    //
    // Sub-record (local tag 0x20 / regular param tag 0x21 / var-or-out
    // param tag 0x22):
    //   <tag> <namelen> <name> <body>
    //
    // Body has two shapes (discriminator: which pair of bytes is 00 00):
    //   5-byte: <subtag>      0x00 0x00 <marker u8> <off ...>   -- typical
    //   6-byte: <subtag> 0x02 0x00 0x00 <marker u8> <off ...>   -- 'out'
    //                                                              params,
    //                                                              subtag=0xCD
    //
    // The sub-record subtag varies (0x66/0x16/0x26/0x62/0x12/0x22/.../0xCD).
    // The strong invariant is the two 0x00 bytes; we anchor on that, not
    // on the subtag value itself. is_primitive is heuristically
    // `subtag == 0x66`.
    //
    // <off ...> is variable-length, discriminated by the LSB of the
    // first byte:
    //   LSB=0: single byte, value is signed (i8), real rbp-offset
    //          formula is `16 + i8/2` (same as Delphi DWARF emission).
    //   LSB=1: two bytes u16-LE, real rbp-offset formula is
    //          `16 + (u16 - 1)/4`. The tag-bit lets Delphi disambiguate
    //          between a negative i8 (e.g. 0x80) and a large positive
    //          stack offset (e.g. for the 7th+ register/stack-passed
    //          parameter on Win64).
    // We normalise both forms into a stored value such that the
    // downstream `16 + stored/2` formula in main.cpp / dwarf_emitter.cpp
    // keeps working: for 2-byte form we store `(u16 - 1) / 2`.
    procedures_.reserve(150000);
    std::size_t rej_subtag = 0, rej_name = 0, rej_no_subrec = 0, rej_walk = 0;

    // Decoded sub-record. `total_size` is the byte-count of the whole
    // record (tag + namelen + name + body + offset bytes), so callers
    // can advance their cursor by exactly the right amount.
    //
    // Sub-records come in two type-ref flavours, discriminated at the
    // byte after `<subtag> 00 00`:
    //
    //   primitive:    <subtag> 00 00 <marker u8>  <offset 1B|2B>
    //                                ^^^^^^^^^^^
    //                                small even byte (Integer=0x02,
    //                                Double=0x04, ...) -> `marker`
    //                                set, `type_hash` left at 0.
    //
    //   non-primitive <subtag> 00 00 <type_hash u16 LE> <offset 1B|2B>
    //                                ^^^^^^^^^^^^^^^^^
    //                                first byte fails the primitive
    //                                test (large or odd) -> read 2
    //                                bytes as type_hash, `marker`
    //                                left at 0. type_hash is the
    //                                own_hash of an aggregate in
    //                                this unit (see Phase B.1).
    //
    // is_primitive captures which branch parseSub took, so callers
    // don't have to redo the discrimination on subtag (which is
    // 0x66 in both flavours and not reliable on its own).
    struct SubRec {
        bool          ok            = false;
        std::uint8_t  tag           = 0;     // 0x20 local / 0x21 param / 0x22 var-param
        std::uint8_t  namelen       = 0;
        std::size_t   name_at       = 0;     // index of first name byte
        std::uint8_t  subtag        = 0;
        std::uint8_t  marker        = 0;     // primitive marker (only when is_primitive)
        std::uint16_t type_hash     = 0;     // aggregate own_hash (only when !is_primitive)
        bool          is_primitive  = false;
        std::int32_t  stored_off    = 0;     // normalised for `16 + s/2` formula
        std::size_t   total_size    = 0;
    };
    auto parseSub = [&](std::size_t s) -> SubRec {
        SubRec r{};
        if (s + 8 >= buf.size()) return r;
        const auto tag = static_cast<std::uint8_t>(buf[s]);
        if (tag != kRecordTagVariable && tag != kRecordTagParam
            && tag != kRecordTagVarParam) return r;
        const auto nl = static_cast<std::uint8_t>(buf[s + 1]);
        if (nl == 0 || nl > 64) return r;
        const std::size_t bodyAt = s + 2 + nl;
        if (bodyAt + 5 >= buf.size()) return r;
        // Find marker position: either after 1-byte subtag + "00 00"
        // (markerAt = bodyAt + 3) or after 2-byte subtag + "00 00"
        // (markerAt = bodyAt + 4, used by 'out' params).
        std::size_t markerAt;
        if (static_cast<std::uint8_t>(buf[bodyAt + 1]) == 0 &&
            static_cast<std::uint8_t>(buf[bodyAt + 2]) == 0) {
            markerAt = bodyAt + 3;
        } else if (static_cast<std::uint8_t>(buf[bodyAt + 2]) == 0 &&
                   static_cast<std::uint8_t>(buf[bodyAt + 3]) == 0) {
            markerAt = bodyAt + 4;
        } else {
            return r;
        }
        if (markerAt + 1 >= buf.size()) return r;
        if (!isPrintableName(buf.data() + s + 2, nl)) return r;

        r.tag       = tag;
        r.namelen   = nl;
        r.name_at   = s + 2;
        r.subtag    = static_cast<std::uint8_t>(buf[bodyAt]);

        // Type-ref discrimination. Two RSM-format generations coexist
        // in our fixture set:
        //
        //   OLD (e.g. two_units.rsm built with Delphi pre-10.x):
        //     subtag 0x62 + <marker u8> for non-primitives;
        //     subtag 0x66 + <marker u8> for primitives.
        //     The marker is a 1-byte per-unit type id.
        //
        //   NEW (e.g. records.rsm built with current Delphi):
        //     subtag 0x66 for BOTH, with the discriminator pushed
        //     into the byte at markerAt: small-even = primitive
        //     marker (1 byte); else a 2-byte composite hash. This
        //     matches the global-variable record's layout.
        //
        // Dispatch order: subtag != 0x66 is a hard signal of OLD
        // non-primitive. subtag == 0x66 may be either generation;
        // discriminate via markerAt[0] like the globals parser does.
        const auto mb0 = static_cast<std::uint8_t>(buf[markerAt]);
        std::size_t offsetAt;
        if (r.subtag != kVarPayloadSubTag0) {
            // OLD non-primitive form. 1-byte marker (per-unit type
            // id), no separate type_hash.
            r.is_primitive = false;
            r.marker       = mb0;
            r.type_hash    = 0;
            offsetAt       = markerAt + 1;
        } else {
            const bool primitive_marker =
                (mb0 != 0 && mb0 < 0x40 && (mb0 & 0x01) == 0);
            if (primitive_marker) {
                r.is_primitive = true;
                r.marker       = mb0;
                r.type_hash    = 0;
                offsetAt       = markerAt + 1;
            } else {
                if (markerAt + 2 >= buf.size()) return r;
                r.is_primitive = false;
                r.marker       = 0;
                r.type_hash    = static_cast<std::uint16_t>(mb0)
                              | (static_cast<std::uint16_t>(
                                    static_cast<std::uint8_t>(buf[markerAt + 1]))
                                 << 8);
                offsetAt       = markerAt + 2;
            }
        }
        if (offsetAt >= buf.size()) return r;

        const auto b0 = static_cast<std::uint8_t>(buf[offsetAt]);
        if ((b0 & 1u) == 0u) {
            const auto iv = static_cast<std::int8_t>(b0);
            r.stored_off = static_cast<std::int32_t>(iv);
            r.total_size = (offsetAt + 1) - s;
        } else {
            if (offsetAt + 2 >= buf.size()) return r;
            const auto b1 = static_cast<std::uint8_t>(buf[offsetAt + 1]);
            const std::uint16_t v =
                  static_cast<std::uint16_t>(b0)
                | (static_cast<std::uint16_t>(b1) << 8);
            // Empirical formula validated against the disassembled
            // addresses of examples/05_types ProbeLocals's string /
            // bool / char locals (all of which use the 2-byte form
            // because their negative-from-top-of-frame offset doesn't
            // fit a signed i8):
            //
            //   real_offset = sub_rsp + ((int16) v - 1) / 4
            //
            // i.e. v is signed (typical raw values are 0xF9..0xFE in
            // the high byte, decoding to -200 .. -2000 range). We
            // normalise to `stored_off = ((int16) v - 1) / 2` so the
            // downstream `sub_rsp + stored_off / 2` formula in
            // main.cpp produces the right address. The earlier
            // unsigned read decoded 0xFE61 as +65121 which sent the
            // 2-byte-form locals well outside any real frame slot.
            const std::int32_t sv =
                static_cast<std::int16_t>(v);
            r.stored_off = (sv - 1) / 2;
            r.total_size = (offsetAt + 2) - s;
        }
        r.ok = true;
        return r;
    };
    auto scanProcedures = [&]() {
        const std::size_t pscanStart = std::min<std::size_t>(0x426, buf.size());
        std::size_t pi = pscanStart;
        while (pi + 16 < buf.size()) {
            if (static_cast<std::uint8_t>(buf[pi]) != kRecordTagFunction) {
                ++pi;
                continue;
            }
            const auto fnameLen = static_cast<std::uint8_t>(buf[pi + 1]);
            if (fnameLen < 2 || fnameLen > 200) { ++pi; continue; }
            const std::size_t fnameStart = pi + 2;
            const std::size_t headEnd    = fnameStart + fnameLen;
            if (headEnd + 14 >= buf.size()) { ++pi; continue; }
            const auto fnSubTag = static_cast<std::uint8_t>(buf[headEnd]);
            const bool isNested = fnSubTag == kFunctionSubTagNested;
            if (fnSubTag != kFunctionSubTag0
                && fnSubTag != kFunctionSubTag1
                && fnSubTag != kFunctionSubTag2
                && fnSubTag != kFunctionSubTag3
                && !isNested) {
                ++pi;
                ++rej_subtag;
                continue;
            }
            // Most flavours require two zero bytes after the subtag
            // (the byte pair distinguishes proc records from random
            // data). The nested-function flavour uses two non-zero
            // bytes (`02 10` in our samples) -- skip the check for it
            // and rely on the printable-name + sub-record-scan checks
            // below to filter out false positives.
            if (!isNested &&
                (static_cast<std::uint8_t>(buf[headEnd + 1]) != 0x00 ||
                 static_cast<std::uint8_t>(buf[headEnd + 2]) != 0x00)) {
                ++pi;
                ++rej_subtag;
                continue;
            }
            if (!isPrintableName(buf.data() + fnameStart, fnameLen)) {
                ++pi;
                ++rej_name;
                continue;
            }

            // VA encoding differs by subtag.
            //   0x80/0xA0/0xE0: a 4-byte name-hash precedes the VA;
            //                   VA bytes at headEnd + 7.
            //   0x20:           no hash, VA bytes immediately follow
            //                   the subtag triple; VA bytes at headEnd + 3.
            //   0x41:           nested-function flavour, 3 mystery
            //                   bytes after the subtag (likely a
            //                   parent-frame hash or flags), then VA
            //                   at headEnd + 4.
            // All forms store the VA as a u32 left-shifted by 4 (low
            // nibble unused). The shorter 0x20 layout is what modern
            // Delphi (10.x) emits for user-code class methods.
            const std::size_t va_off = (fnSubTag == kFunctionSubTag3)
                                       ? headEnd + 3
                                       : (isNested ? headEnd + 4
                                                   : headEnd + 7);
            const std::uint32_t shifted = readU32LE(buf.data() + va_off);
            const std::uint64_t va = static_cast<std::uint64_t>(shifted) >> 4;

            // The trailer between the VA and the first sub-record (or
            // end marker) is variable-length, 3..30 bytes empirically.
            // Scan forward to locate either:
            //   - a well-formed sub-record start (0x20/0x21 + valid
            //     namelen + printable name + 0x00 0x00 at expected
            //     offset), or
            //   - a real 0x63 end marker (whose next byte starts a
            //     recognizable record; bare 0x63 bytes inside the
            //     trailer are common false positives).
            const std::size_t scanFrom = va_off + 4;
            const std::size_t scanCap  = buf.size() > 8 ? buf.size() - 8 : 0;
            const std::size_t scanTo   = std::min(scanFrom + 96, scanCap);
            std::size_t firstSub = SIZE_MAX;
            bool endedImmediately = false;
            for (std::size_t t = scanFrom; t < scanTo; ++t) {
                const auto tb = static_cast<std::uint8_t>(buf[t]);
                if (tb == kRecordTagParam || tb == kRecordTagVariable
                    || tb == kRecordTagVarParam) {
                    if (parseSub(t).ok) {
                        firstSub = t;
                        break;
                    }
                } else if (tb == kFunctionEndMarker && t + 1 < buf.size()) {
                    const auto nx = static_cast<std::uint8_t>(buf[t + 1]);
                    if (nx == kRecordTagFunction || nx == kRecordTagVariable ||
                        nx == kRecordTagPrimitive || nx == kRecordTerminator ||
                        nx == kTypeIdMarker0) {
                        firstSub = t;
                        endedImmediately = true;
                        break;
                    }
                }
            }
            if (firstSub == SIZE_MAX) {
                ++pi;
                ++rej_no_subrec;
                continue;
            }

            ProcedureRecord proc;
            proc.name             = std::string(buf.data() + fnameStart, fnameLen);
            proc.address          = va;
            proc.file_offset      = pi;
            proc.has_static_link  = isNested;

            if (endedImmediately) {
                proc.file_offset_end = firstSub + 1;
                procedures_.push_back(std::move(proc));
                pi = firstSub + 1;
                continue;
            }

            std::size_t s = firstSub;
            bool ok = true;
            while (s + 8 < buf.size()) {
                // Between consecutive sub-records the encoding sometimes
                // carries an extra 1-2 byte attribute/padding (e.g.
                // `var` parameters in real Delphi binaries). Allow a
                // small scan-forward to re-anchor on the next valid
                // sub-record header or end marker. Without this we lose
                // ~80k procs on AdvPCB.
                const std::size_t reAnchorCap =
                    std::min(s + 6, buf.size() > 8 ? buf.size() - 8 : 0);
                while (s < reAnchorCap) {
                    const auto tb = static_cast<std::uint8_t>(buf[s]);
                    if (tb == kFunctionEndMarker && s + 1 < buf.size()) {
                        const auto nx = static_cast<std::uint8_t>(buf[s + 1]);
                        if (nx == kRecordTagFunction || nx == kRecordTagVariable ||
                            nx == kRecordTagPrimitive || nx == kRecordTerminator ||
                            nx == kTypeIdMarker0) {
                            break;
                        }
                    }
                    if (tb == kRecordTagParam || tb == kRecordTagVariable
                        || tb == kRecordTagVarParam) {
                        if (parseSub(s).ok) break;
                    }
                    ++s;
                }
                if (s + 8 >= buf.size()) break;  // EOF -- keep what we got

                const auto tag = static_cast<std::uint8_t>(buf[s]);
                if (tag == kFunctionEndMarker) { ++s; break; }
                // Real Delphi RSMs use additional sub-record tags
                // (e.g. 0x23 / 0x25) for return-value metadata and
                // enum-entries. We don't decode them yet, so we stop
                // the param/local walk gracefully on any unknown
                // tag rather than discarding the whole proc.
                if (tag != kRecordTagParam && tag != kRecordTagVariable
                    && tag != kRecordTagVarParam) {
                    break;
                }
                const SubRec sub = parseSub(s);
                if (!sub.ok) break;

                Variable sv{};
                sv.name           = std::string(buf.data() + sub.name_at,
                                                sub.namelen);
                sv.address        = 0;
                sv.stack_offset   = sub.stored_off;
                // is_primitive now comes from parseSub's discriminator
                // (small-even byte at the marker position), NOT from
                // the subtag value -- non-primitive sub-records share
                // subtag 0x66 with primitive ones and would otherwise
                // be misclassified. type_hash carries the full 2-byte
                // aggregate own_hash when non-primitive (Phase B.3).
                sv.is_primitive   = sub.is_primitive;
                sv.type_marker    = sub.is_primitive ? sub.marker : 0;
                sv.inline_type_id = sub.is_primitive ? 0 : sub.type_hash;
                sv.has_trailer    = false;
                sv.trailer_type_id = 0;
                sv.file_offset    = s;

                if (sub.tag == kRecordTagVariable) {
                    proc.locals.push_back(std::move(sv));
                } else {
                    proc.params.push_back(std::move(sv));
                }
                s += sub.total_size;
            }

            if (ok) {
                proc.file_offset_end = s;
                procedures_.push_back(std::move(proc));
                pi = s;
            } else {
                ++pi;
                ++rej_walk;
            }
        }
    };
    scanProcedures();
    std::fprintf(stderr,
                 "[rsm] proc scan: %zu found "
                 "(rejected subtag=%zu name=%zu no_sub=%zu walk=%zu)\n",
                 procedures_.size(), rej_subtag, rej_name,
                 rej_no_subrec, rej_walk);

    // Quick predicate: does the given file offset fall inside any
    // procedure record we just parsed?  procedures_ is in increasing
    // file_offset order by construction, so binary search is sound.
    // (Linear search was fine with ~6k procs, but at 130k+ it dominated
    //  the variable-scan loop.)
    auto isInsideProcedure = [&](std::size_t off) {
        auto it = std::upper_bound(
            procedures_.begin(), procedures_.end(), off,
            [](std::size_t v, const ProcedureRecord& p) {
                return v < p.file_offset;
            });
        if (it == procedures_.begin()) return false;
        --it;
        return off < it->file_offset_end;
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

    // -- Per-unit type tables -----------------------------------------
    //
    // Variables / params / locals carry a per-unit `type_marker` (an
    // even byte, 0x02 / 0x04 / 0x06 / ...) that indexes the unit's
    // primary type table -- a run of records, one per distinct Pascal
    // type used in the unit, in source-declaration order:
    //
    //   0x66 <namelen u8> <Pascal name> <4-byte hash>
    //
    // The first record's `name` corresponds to marker 0x02, the second
    // to 0x04, etc. (so `index = marker/2 - 1`, `marker = 2*(index+1)`).
    //
    // Locating each unit's primary table:
    //   1. Find every "unit anchor" -- `02 <namelen> <unit name>`
    //      followed (within ~200 bytes) by `02 70 <namelen> <source
    //      file ending in .pas / .dpr / .inc / .tmp>`.
    //   2. For each anchor, the next-following 0x66-run is the unit's
    //      primary type table.
    //   3. For each variable, the enclosing unit is the closest-prior
    //      anchor; look up its primary table and decode the marker.
    //
    // Variables / functions in units whose anchor we don't find (some
    // RTL units use a layout the strict heuristic above doesn't match)
    // simply get no `pascal_type` -- callers fall back to byte-size-
    // based typing for those.
    auto isPrintAscii = [&](const char* p, std::size_t n) {
        for (std::size_t k = 0; k < n; ++k) {
            const auto c = static_cast<std::uint8_t>(p[k]);
            if (c < 0x20 || c >= 0x7F) return false;
        }
        return true;
    };
    auto endsWith = [](const char* p, std::size_t n, const char* suf,
                       std::size_t sufN) {
        return n >= sufN &&
               std::memcmp(p + n - sufN, suf, sufN) == 0;
    };

    struct UnitAnchor { std::size_t file_offset; std::string name; };
    // Local during the primary-type-table scan; mirrored into the
    // member unit_anchor_offsets_ at the end so later passes (e.g.
    // the aggregate parser, the per-unit resolver, downstream
    // callers via unitAnchorFor) can use it.
    std::vector<UnitAnchor> unit_anchors;
    const std::size_t kVarPayloadHdr = 0x420;
    for (std::size_t a = kVarPayloadHdr; a + 60 < buf.size(); ) {
        if (static_cast<std::uint8_t>(buf[a]) != 0x02) { ++a; continue; }
        const auto nl = static_cast<std::uint8_t>(buf[a + 1]);
        if (nl < 2 || nl > 40 ||
            !isPrintAscii(buf.data() + a + 2, nl)) {
            ++a; continue;
        }
        // Look for `02 70 <fnl> <printable>.{pas,dpr,inc,tmp}` within
        // the next 200 bytes -- the source-file signature.
        const std::size_t scan_lo = a + 2 + nl;
        const std::size_t scan_hi = std::min(buf.size(), scan_lo + 200);
        bool matched = false;
        for (std::size_t s = scan_lo; s + 8 < scan_hi; ++s) {
            if (static_cast<std::uint8_t>(buf[s]) != 0x02 ||
                static_cast<std::uint8_t>(buf[s + 1]) != 0x70) continue;
            const auto fnl = static_cast<std::uint8_t>(buf[s + 2]);
            if (fnl < 4 || fnl > 80) continue;
            if (s + 3 + fnl > buf.size()) continue;
            const char* fp = buf.data() + s + 3;
            if (!isPrintAscii(fp, fnl)) continue;
            if (endsWith(fp, fnl, ".pas", 4) ||
                endsWith(fp, fnl, ".dpr", 4) ||
                endsWith(fp, fnl, ".inc", 4) ||
                endsWith(fp, fnl, ".tmp", 4)) {
                matched = true;
                break;
            }
        }
        if (matched) {
            unit_anchors.push_back({
                a, std::string(buf.data() + a + 2, nl)});
            a += 2 + nl;
        } else {
            ++a;
        }
    }

    // Build a primary type table per unit anchor by scanning the
    // bytes between this anchor and the next one. The unit's type
    // table is a sequence of `0x66 <namelen> <name> <4-byte hash>`
    // records interleaved with `0x67`-tagged function / import
    // references (same record shape, different tag) -- we collect
    // the 0x66's in source-declaration order and skip past the 0x67's.
    // marker N (always even, >= 0x02) is the (N/2)-th entry.
    struct TypeTable {
        std::size_t              start;
        std::vector<std::string> names;
    };
    std::vector<TypeTable> primary_tables;
    primary_tables.reserve(unit_anchors.size());
    for (std::size_t ua = 0; ua < unit_anchors.size(); ++ua) {
        const std::size_t lo = unit_anchors[ua].file_offset;
        const std::size_t hi = (ua + 1 < unit_anchors.size())
            ? unit_anchors[ua + 1].file_offset
            : buf.size();

        // Hunt for the first 0x66 / 0x67 record after the anchor's
        // unit-name + source-file headers (skip the first ~250 bytes
        // of fixed-shape headers so we don't latch onto stray 0x66
        // bytes inside the path string).
        TypeTable tab;
        tab.start = lo;
        const std::size_t scan_lo = lo + 0x20;  // past unit name etc.
        for (std::size_t cur = scan_lo; cur + 10 < hi; ) {
            const auto b0 = static_cast<std::uint8_t>(buf[cur]);
            // Both 0x66 and 0x67 records share the
            // `<tag> <namelen> <name> <4-byte hash>` shape.
            if (b0 != kVarPayloadSubTag0 && b0 != 0x67) {
                // First record we don't recognise -- either we
                // haven't reached the table yet (try next byte) or
                // we ran off the end (give up after ~500 bytes
                // without finding a 0x66 entry).
                if (!tab.names.empty()) break;
                if (cur - scan_lo > 500) break;
                ++cur;
                continue;
            }
            const auto c_nl = static_cast<std::uint8_t>(buf[cur + 1]);
            if (c_nl < 2 || c_nl > 80 ||
                cur + 2 + c_nl + 4 > hi ||
                !isPrintAscii(buf.data() + cur + 2, c_nl)) {
                if (!tab.names.empty()) break;
                ++cur;
                continue;
            }
            if (b0 == kVarPayloadSubTag0) {
                tab.names.emplace_back(buf.data() + cur + 2, c_nl);
            }
            cur += 2 + c_nl + 4;
        }
        primary_tables.push_back(std::move(tab));
    }

    // Map anchor offset -> primary table (by index in primary_tables;
    // unit_anchors and primary_tables are kept aligned).
    std::unordered_map<std::size_t /*anchor offset*/,
                       const TypeTable*> primary_table;
    for (std::size_t k = 0; k < unit_anchors.size(); ++k) {
        if (!primary_tables[k].names.empty()) {
            primary_table[unit_anchors[k].file_offset] = &primary_tables[k];
        }
    }

    // Resolve pascal_type for one Variable using its enclosing unit's
    // primary table.
    auto resolvePascalType = [&](Variable& v) {
        if (!v.is_primitive || v.type_marker == 0
            || (v.type_marker & 1) != 0) return;
        const std::size_t idx =
            static_cast<std::size_t>(v.type_marker) / 2;
        if (idx == 0) return;
        auto it = std::upper_bound(
            unit_anchors.begin(), unit_anchors.end(), v.file_offset,
            [](std::size_t v_, const UnitAnchor& a) {
                return v_ < a.file_offset;
            });
        if (it == unit_anchors.begin()) return;
        --it;
        auto pit = primary_table.find(it->file_offset);
        if (pit == primary_table.end() || pit->second == nullptr) return;
        const auto& names = pit->second->names;
        if (idx > names.size()) return;
        v.pascal_type = names[idx - 1];
    };
    for (auto& v : variables_) resolvePascalType(v);
    for (auto& p : procedures_) {
        for (auto& v : p.params) resolvePascalType(v);
        for (auto& v : p.locals) resolvePascalType(v);
    }
    std::fprintf(stderr,
                 "[rsm] type tables: %zu anchors, %zu primaries, "
                 "%zu globals typed\n",
                 unit_anchors.size(), primary_table.size(),
                 [&] {
                     std::size_t n = 0;
                     for (const auto& v : variables_)
                         if (!v.pascal_type.empty()) ++n;
                     return n;
                 }());

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

    // Mirror unit_anchors offsets into a member sorted vector so the
    // aggregate parser (below) + downstream callers (unitAnchorFor
    // and the per-unit aggregate lookup) can do an upper_bound query
    // against it.
    unit_anchor_offsets_.clear();
    unit_anchor_offsets_.reserve(unit_anchors.size());
    for (const auto& ua : unit_anchors) {
        unit_anchor_offsets_.push_back(ua.file_offset);
    }
    std::sort(unit_anchor_offsets_.begin(), unit_anchor_offsets_.end());

    // Mirror the per-unit primary type-table name list into a
    // member map keyed by anchor offset. decorateTypes() consults it
    // when resolving a field's primitive_marker to a Pascal type
    // name.
    primary_table_by_anchor_.clear();
    for (std::size_t k = 0; k < unit_anchors.size(); ++k) {
        if (k >= primary_tables.size()) break;
        const auto& tt = primary_tables[k];
        if (tt.names.empty()) continue;
        primary_table_by_anchor_[unit_anchors[k].file_offset] = tt.names;
    }

    // -- Aggregate types (Step 11b Phase B.1) -------------------------
    //
    // Walk the metadata stream collecting 0x2a / 0x2c / 0x25 records
    // into `aggregates_`. See rsm-format.txt 2026-05-28 entries for
    // the full encoding.
    //
    // Two-pass approach (the records live in two physically separate
    // clusters within the unit's metadata stream, linked by
    // parent_hash):
    //   pass 1 -- collect 0x2a records into aggregates_ keyed by
    //             own_hash; collect dangling 0x2c / 0x25 records
    //             into temporary maps keyed by parent_hash.
    //   pass 2 -- attach the dangling records to their parent
    //             aggregate; infer kind (record / class / enum)
    //             from the attached storage bytes / entries.
    //
    // We DELIBERATELY don't trust the 0x2a record's `kind` byte to
    // discriminate -- empirically TBig (record with 40 fields) and
    // TColor (enum) share the same kind byte 0x00, so kind must be
    // inferred from what attaches to the type.
    {
        std::unordered_map<std::uint16_t,
                           std::vector<FieldEntry>> pending_fields;
        std::unordered_map<std::uint16_t,
                           std::vector<EnumEntry>>  pending_enums;
        // For class header back-references (base_hash, total_size).
        // The class header's `47 00 10 00 00 <own_hash u16>` pattern
        // lets us pick out base-class refs even though they're in a
        // separate physical position from the 0x2a record.
        struct ClassHeader {
            std::uint16_t own_hash;
            std::uint16_t base_hash;     // 0 if "no explicit base"
        };
        std::vector<ClassHeader> class_headers;

        auto readU16LE = [&](std::size_t off) -> std::uint16_t {
            return static_cast<std::uint8_t>(buf[off])
                 | (static_cast<std::uint8_t>(buf[off + 1]) << 8);
        };

        const std::size_t scan_lo = std::min<std::size_t>(0x420, buf.size());
        std::size_t i = scan_lo;
        while (i + 8 < buf.size()) {
            const auto tag = static_cast<std::uint8_t>(buf[i]);

            // 0x2a -- type record: 2a <namelen> <name> <kind u8> 00 00 <own_hash u16> [extras]
            if (tag == 0x2a) {
                const auto nl = static_cast<std::uint8_t>(buf[i + 1]);
                if (nl < 2 || nl > 64
                    || i + 2 + nl + 6 > buf.size()) { ++i; continue; }
                if (!isPrintableName(buf.data() + i + 2, nl)) { ++i; continue; }
                // Validate the `<kind> 00 00` shape at +2+nl.
                const std::size_t at = i + 2 + nl;
                if (static_cast<std::uint8_t>(buf[at + 1]) != 0x00
                    || static_cast<std::uint8_t>(buf[at + 2]) != 0x00) {
                    ++i; continue;
                }
                AggregateType a;
                a.name        = std::string(buf.data() + i + 2, nl);
                a.own_hash    = readU16LE(at + 3);
                a.file_offset = i;
                // Pin the aggregate to its enclosing unit anchor.
                // Closest-prior anchor via upper_bound on sorted
                // offsets (Phase B.4). Without a unit anchor the
                // aggregate is unscoped; downstream lookups fall
                // back to the global last-wins index.
                auto uit = std::upper_bound(
                    unit_anchor_offsets_.begin(),
                    unit_anchor_offsets_.end(),
                    static_cast<std::uint64_t>(i));
                a.unit_anchor_offset =
                    (uit == unit_anchor_offsets_.begin())
                        ? 0
                        : *(uit - 1);
                // Provisional kind = Unknown; refined in pass 2.
                aggregates_.push_back(std::move(a));
                i = at + 5;
                continue;
            }

            // 0x2c -- field record: 2c <namelen> <name> 00 <storage> 00
            //                       <type-ref 1/2B> <offset 1/2B>
            //                       <internals 8/13B> <parent_hash u16> ff
            if (tag == 0x2c) {
                const auto nl = static_cast<std::uint8_t>(buf[i + 1]);
                if (nl < 1 || nl > 64
                    || i + 2 + nl + 6 > buf.size()) { ++i; continue; }
                if (!isPrintableName(buf.data() + i + 2, nl)) { ++i; continue; }
                const std::size_t at = i + 2 + nl;
                // Anchor on the `00 <storage> 00` prefix.
                if (static_cast<std::uint8_t>(buf[at]) != 0x00
                    || static_cast<std::uint8_t>(buf[at + 2]) != 0x00) {
                    ++i; continue;
                }
                const std::uint8_t storage =
                    static_cast<std::uint8_t>(buf[at + 1]);
                std::size_t cur = at + 3;
                // Type-ref discriminator: small even byte (< 0x40) =
                // primitive marker; else 2-byte hash. We don't dare
                // assume; check both bytes.
                FieldEntry fe;
                fe.name    = std::string(buf.data() + i + 2, nl);
                fe.storage = storage;

                const auto b0 = static_cast<std::uint8_t>(buf[cur]);
                bool is_primitive_ref = (b0 != 0
                                         && b0 < 0x40
                                         && (b0 & 0x01) == 0);
                if (is_primitive_ref) {
                    fe.primitive_marker = b0;
                    cur += 1;
                } else {
                    if (cur + 2 > buf.size()) { ++i; continue; }
                    fe.type_hash = readU16LE(cur);
                    cur += 2;
                }

                // Offset: 1-byte form if LSB=0, else 2-byte LE u16
                // decoded as `real = (v - 1) / 4`.
                if (cur >= buf.size()) { ++i; continue; }
                const auto off_b0 = static_cast<std::uint8_t>(buf[cur]);
                if ((off_b0 & 0x01) == 0) {
                    fe.offset = off_b0 / 2;
                    cur += 1;
                } else {
                    if (cur + 2 > buf.size()) { ++i; continue; }
                    const std::uint16_t v = readU16LE(cur);
                    fe.offset = (static_cast<std::uint32_t>(v) - 1) / 4;
                    cur += 2;
                }

                // Walk to terminating `ff`. The `<parent_hash u16> ff`
                // sequence is canonical -- internals are 8 or 13 bytes
                // depending on the type-ref form, but defensively we
                // scan up to 24 bytes ahead for an `ff` and capture the
                // u16 immediately before it as parent_hash.
                std::size_t walk_end = std::min(cur + 24, buf.size());
                std::size_t ff_at = SIZE_MAX;
                for (std::size_t j = cur; j < walk_end; ++j) {
                    if (static_cast<std::uint8_t>(buf[j]) == 0xff) {
                        ff_at = j;
                        break;
                    }
                }
                if (ff_at == SIZE_MAX || ff_at < cur + 2) { ++i; continue; }
                const std::uint16_t parent = readU16LE(ff_at - 2);
                pending_fields[parent].push_back(std::move(fe));
                i = ff_at + 1;
                continue;
            }

            // 0x25 -- enum entry: 25 <namelen> <name> 0a 00 00 <parent_hash u16> 00 00 <ord_x2 u8>
            if (tag == 0x25) {
                const auto nl = static_cast<std::uint8_t>(buf[i + 1]);
                if (nl < 1 || nl > 64
                    || i + 2 + nl + 9 > buf.size()) { ++i; continue; }
                if (!isPrintableName(buf.data() + i + 2, nl)) { ++i; continue; }
                const std::size_t at = i + 2 + nl;
                // Anchor on `0a 00 00`.
                if (static_cast<std::uint8_t>(buf[at])     != 0x0a
                    || static_cast<std::uint8_t>(buf[at + 1]) != 0x00
                    || static_cast<std::uint8_t>(buf[at + 2]) != 0x00) {
                    ++i; continue;
                }
                const std::uint16_t parent = readU16LE(at + 3);
                // Validate the trailing `00 00 <ord_x2>` shape.
                if (static_cast<std::uint8_t>(buf[at + 5]) != 0x00
                    || static_cast<std::uint8_t>(buf[at + 6]) != 0x00) {
                    ++i; continue;
                }
                const auto ord_x2 = static_cast<std::uint8_t>(buf[at + 7]);
                EnumEntry e;
                e.name    = std::string(buf.data() + i + 2, nl);
                e.ordinal = static_cast<std::int64_t>(ord_x2) / 2;
                pending_enums[parent].push_back(std::move(e));
                i = at + 8;
                continue;
            }

            // Class header signature: 47 00 10 00 00 <own_hash u16>
            // Pull out the base_hash that sits ~12 bytes later (at the
            // fixed offset just before the `25 3f 00` field-list
            // anchor). See rsm-format.txt 2026-05-28 inheritance entry.
            if (tag == 0x47 && i + 24 < buf.size()
                && static_cast<std::uint8_t>(buf[i + 1]) == 0x00
                && static_cast<std::uint8_t>(buf[i + 2]) == 0x10
                && static_cast<std::uint8_t>(buf[i + 3]) == 0x00
                && static_cast<std::uint8_t>(buf[i + 4]) == 0x00) {
                ClassHeader ch{};
                ch.own_hash  = readU16LE(i + 5);
                // The 11-byte vtable-size block lives at +7..+17,
                // then the base-class 2-byte slot at +18..+19.
                ch.base_hash = readU16LE(i + 18);
                class_headers.push_back(ch);
                i += 7;
                continue;
            }

            ++i;
        }

        // Build the own_hash -> aggregate index map.
        //
        // own_hash is unit-local, not globally unique -- RTL units can
        // declare types with the same hash as our user units (TColor
        // in records.dpr collides with at least one System-unit type
        // at hash 0x1a6d). Two indexes:
        //
        //   aggr_by_hash_       -- last-wins by hash alone. Used when
        //                          caller has no unit context.
        //                          (User units link last, so the
        //                          user aggregate wins on practical
        //                          fixtures.)
        //
        //   aggr_by_unit_hash_  -- precise by (unit_anchor_offset,
        //                          hash). Used when the caller knows
        //                          which unit owns the lookup. The
        //                          pascal_type resolver below uses
        //                          this when a variable's file_offset
        //                          falls inside a unit anchor.
        aggr_by_hash_.reserve(aggregates_.size());
        aggr_by_unit_hash_.reserve(aggregates_.size());
        for (std::size_t k = 0; k < aggregates_.size(); ++k) {
            const auto& a = aggregates_[k];
            aggr_by_hash_[a.own_hash] = k;
            if (a.unit_anchor_offset != 0) {
                aggr_by_unit_hash_[
                    UnitHashKey{a.unit_anchor_offset, a.own_hash}] = k;
            }
        }

        // Attach pending fields / enums by parent_hash, then classify.
        for (auto& [h, v] : pending_fields) {
            auto it = aggr_by_hash_.find(h);
            if (it == aggr_by_hash_.end()) continue;
            auto& a = aggregates_[it->second];
            a.fields = std::move(v);
        }
        for (auto& [h, v] : pending_enums) {
            auto it = aggr_by_hash_.find(h);
            if (it == aggr_by_hash_.end()) continue;
            auto& a = aggregates_[it->second];
            a.enum_entries = std::move(v);
        }
        // Attach class header data (base_hash) when own_hash matches.
        for (const auto& ch : class_headers) {
            auto it = aggr_by_hash_.find(ch.own_hash);
            if (it == aggr_by_hash_.end()) continue;
            auto& a = aggregates_[it->second];
            // Only treat as a base ref if it matches another local
            // aggregate's own_hash. The "no explicit base" sentinel
            // (e.g. TShape's `12 00`) won't match.
            if (ch.base_hash != 0
                && aggr_by_hash_.count(ch.base_hash) != 0) {
                a.base_hash = ch.base_hash;
            }
        }

        // Build a set of "is class header" hashes -- if a 0x2a
        // aggregate's own_hash appears here, it's a class (the
        // `47 00 10 00 00 <own_hash>` header is class-exclusive;
        // records don't carry a vtable so they don't emit one).
        // This is the only RELIABLE way to discriminate record from
        // class -- field-storage byte encodes visibility, not kind
        // (TBag with public fields has storage 0x02 just like a
        // record's members).
        std::unordered_set<std::uint16_t> class_hashes;
        class_hashes.reserve(class_headers.size());
        for (const auto& ch : class_headers) class_hashes.insert(ch.own_hash);

        // Classification:
        //   - has enum_entries                          -> Enum
        //   - own_hash in class_hashes                  -> Class
        //   - has fields with storage 0xa0              -> PackedRecord
        //   - has fields (default)                      -> Record
        //   - no fields, no enum entries, no class hdr  -> Unknown
        //                                                  (Set candidate)
        for (auto& a : aggregates_) {
            if (!a.enum_entries.empty()) {
                a.kind = AggregateKind::Enum;
                continue;
            }
            if (class_hashes.count(a.own_hash)) {
                a.kind = AggregateKind::Class;
                continue;
            }
            bool any_packed = false;
            for (const auto& fe : a.fields) {
                if (fe.storage == 0xa0) { any_packed = true; break; }
            }
            if (any_packed)            a.kind = AggregateKind::PackedRecord;
            else if (!a.fields.empty()) a.kind = AggregateKind::Record;
            else                       a.kind = AggregateKind::Unknown;
        }

        // Best-effort total_size: sum of largest (offset + assumed
        // size). For packed we trust the offset+marker_size; for
        // others we just take last_offset + 8 as a conservative
        // approximation (downstream code will tighten this when
        // it actually emits TPI records and knows primitive widths).
        // This is just informational for now.
        for (auto& a : aggregates_) {
            std::uint32_t hi = 0;
            for (const auto& fe : a.fields) {
                if (fe.offset > hi) hi = fe.offset;
            }
            a.total_size = (hi == 0 && a.fields.empty()) ? 0 : hi + 8;
        }

        // Stats
        std::size_t rec=0, packed=0, cls=0, enm=0, unk=0;
        std::size_t fields_total = 0, entries_total = 0;
        for (const auto& a : aggregates_) {
            switch (a.kind) {
              case AggregateKind::Record:        ++rec; break;
              case AggregateKind::PackedRecord:  ++packed; break;
              case AggregateKind::Class:         ++cls; break;
              case AggregateKind::Enum:          ++enm; break;
              default:                           ++unk; break;
            }
            fields_total  += a.fields.size();
            entries_total += a.enum_entries.size();
        }
        std::fprintf(stderr,
                     "[rsm] aggregates: %zu total (records %zu, packed %zu, "
                     "classes %zu, enums %zu, unknown %zu); %zu fields, "
                     "%zu enum entries\n",
                     aggregates_.size(), rec, packed, cls, enm, unk,
                     fields_total, entries_total);
    }

    // -- Aggregate-based type resolution (Phase B.2 + B.3) -----------
    //
    // Non-primitive variables / params / locals carry their type's
    // own_hash in inline_type_id (see Variable::inline_type_id in
    // rsm_reader.h). Resolve to Pascal-type name via
    // findAggregateByHash; the existing primitive-typed-via-0x66 path
    // (above) is left alone.
    {
        // Per-unit-first resolution (Phase B.4): if the variable's
        // file_offset falls inside a known unit anchor, look up the
        // aggregate by (unit, hash) so an RTL hash collision can't
        // route to the wrong type. Fall back to global last-wins
        // when no enclosing unit anchor is known (rare; happens for
        // variables that the existing scanner picked up outside any
        // unit boundary -- mostly bogus VAs from prologue bytes that
        // the var_by_va_ filter already drops downstream).
        auto resolveAggrFor = [&](Variable& v) -> bool {
            if (v.is_primitive) return false;
            if (v.inline_type_id == 0) return false;
            if (!v.pascal_type.empty()) return false;
            const std::uint64_t ua = unitAnchorFor(v.file_offset);
            const AggregateType* a = nullptr;
            if (ua != 0) {
                a = findAggregateInUnit(ua, v.inline_type_id);
            }
            if (a == nullptr) {
                // Either no enclosing unit anchor, or this hash
                // simply isn't defined in this unit. Last-wins is
                // the only useful answer left.
                a = findAggregateByHash(v.inline_type_id);
            }
            if (a == nullptr) return false;
            v.pascal_type = a->name;
            return true;
        };
        std::size_t typed_globals = 0;
        std::size_t typed_params  = 0;
        std::size_t typed_locals  = 0;
        for (auto& v : variables_) {
            if (resolveAggrFor(v)) ++typed_globals;
        }
        for (auto& p : procedures_) {
            for (auto& v : p.params) {
                if (resolveAggrFor(v)) ++typed_params;
            }
            for (auto& v : p.locals) {
                if (resolveAggrFor(v)) ++typed_locals;
            }
        }
        std::fprintf(stderr,
                     "[rsm] aggregate-typed: %zu globals, %zu params, "
                     "%zu locals\n",
                     typed_globals, typed_params, typed_locals);
    }

    return true;
}

std::optional<Reader::ResolvedPrimitive>
Reader::resolvePrimitive(std::string_view pascal_name) {
    if (auto* d = lookupPrimitiveDesc(pascal_name)) {
        return ResolvedPrimitive{d->kind, d->byte_size};
    }
    return std::nullopt;
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

const AggregateType* Reader::findAggregateByHash(std::uint16_t hash) const {
    auto it = aggr_by_hash_.find(hash);
    return it == aggr_by_hash_.end() ? nullptr : &aggregates_[it->second];
}

const AggregateType*
Reader::findAggregateInUnit(std::uint64_t unit_anchor_offset,
                            std::uint16_t hash) const {
    if (unit_anchor_offset == 0) return nullptr;
    auto it = aggr_by_unit_hash_.find(UnitHashKey{unit_anchor_offset, hash});
    return it == aggr_by_unit_hash_.end() ? nullptr : &aggregates_[it->second];
}

std::uint64_t Reader::unitAnchorFor(std::uint64_t file_offset) const {
    auto it = std::upper_bound(unit_anchor_offsets_.begin(),
                               unit_anchor_offsets_.end(),
                               file_offset);
    if (it == unit_anchor_offsets_.begin()) return 0;
    return *(it - 1);
}

const std::string* Reader::primitiveNameForMarker(
        std::uint64_t unit_anchor_offset, std::uint8_t marker) const {
    if (unit_anchor_offset == 0) return nullptr;
    if (marker == 0 || (marker & 0x01) != 0) return nullptr;
    auto it = primary_table_by_anchor_.find(unit_anchor_offset);
    if (it == primary_table_by_anchor_.end()) return nullptr;
    const auto idx = static_cast<std::size_t>(marker) / 2;
    if (idx == 0 || idx > it->second.size()) return nullptr;
    return &it->second[idx - 1];
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
        // The aggregate-aware path lives in the PDB pipeline directly
        // (Phase D); the DWARF emitter still needs its array-of-byte
        // fallback here until Phase J takes it over.
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
                // records. PDB pipeline (Phase D) has its own struct
                // synth that goes via PdbInputs::aggregates; DWARF
                // will gain it in Phase J.
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
