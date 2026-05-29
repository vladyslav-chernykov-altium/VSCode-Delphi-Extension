#include "rsm/rsm_reader.h"
#include "rsm/rsm_internal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>

namespace rsm2pdb::rsm {

using namespace detail;

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
void Reader::scanProceduresAndVariables(const std::string& buf) {
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
        //     subtag 0x62 + <marker u8> for non-primitives.
        //     The marker is a 1-byte per-unit type id.
        //
        //   NEW (e.g. records.rsm / AdvPCB.rsm):
        //     subtag 0x66 for BOTH primitive and composite-typed
        //     locals. The form (1-byte marker vs 2-byte hash) AND
        //     the offset form (1-byte vs 2-byte) are encoded by
        //     BODY LENGTH:
        //
        //       primitive 1B marker + 1B offset   -> 5-byte body
        //       primitive 1B marker + 2B offset   -> 6-byte body
        //       composite 2B hash   + 1B offset   -> 6-byte body
        //       composite 2B hash   + 2B offset   -> 7-byte body
        //
        // To pick the right one we look ahead for the next plausible
        // record tag (0x20/0x21/0x22/0x23/0x25/0x28/0x2a/0x63). The
        // earlier "byte < 0x40 && even" heuristic for primitive
        // markers was too restrictive -- AdvPCB's per-unit type
        // tables hand out markers up to ~0x9a (TDynamicString = 0x9a
        // in the PCBCommands_PCB unit), which is well above 0x40.
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
            // Scan forward up to 16 bytes for the next plausible
            // record tag to nail down body length.
            auto isKnownNextTag = [](std::uint8_t b) {
                return b == 0x20 || b == 0x21 || b == 0x22
                    || b == 0x23 || b == 0x25 || b == 0x28
                    || b == 0x2a || b == 0x63;
            };
            const std::size_t maxBodyEnd =
                std::min(bodyAt + 16, buf.size() - 1);
            std::size_t bodyEnd = SIZE_MAX;
            for (std::size_t k = bodyAt + 5; k <= maxBodyEnd; ++k) {
                if (isKnownNextTag(static_cast<std::uint8_t>(buf[k]))) {
                    bodyEnd = k;
                    break;
                }
            }
            if (bodyEnd == SIZE_MAX) return r;
            const std::size_t bodyLen = bodyEnd - bodyAt;
            // Pick form from body length.
            bool composite_form;
            bool two_byte_offset;
            if (bodyLen == 5) {
                composite_form  = false;
                two_byte_offset = false;
            } else if (bodyLen == 7) {
                composite_form  = true;
                two_byte_offset = true;
            } else if (bodyLen == 6) {
                // Ambiguous (primitive 2B-offset vs composite 1B-offset).
                // Disambiguate by LSB of marker byte and of position +4:
                //   - mb0 odd                -> definitely composite
                //     (primitive markers are always even).
                //   - mb0 even AND buf[+4] LSB=1 -> primitive 2B
                //     (offset_lo's LSB=1 is the 2-byte-offset signal).
                //   - else                   -> composite 1B (default;
                //     even-low-byte hashes do exist on RTL types).
                if ((mb0 & 1u) != 0u) {
                    composite_form  = true;
                    two_byte_offset = false;
                } else {
                    const auto b4 =
                        static_cast<std::uint8_t>(buf[bodyAt + 4]);
                    if ((b4 & 1u) != 0u) {
                        composite_form  = false;
                        two_byte_offset = true;
                    } else {
                        composite_form  = true;
                        two_byte_offset = false;
                    }
                }
            } else {
                // Unexpected body length; bail rather than mis-decode.
                return r;
            }
            if (composite_form) {
                if (markerAt + 2 > buf.size()) return r;
                r.is_primitive = false;
                r.marker       = 0;
                r.type_hash    = static_cast<std::uint16_t>(mb0)
                              | (static_cast<std::uint16_t>(
                                    static_cast<std::uint8_t>(buf[markerAt + 1]))
                                 << 8);
                offsetAt       = markerAt + 2;
            } else {
                r.is_primitive = true;
                r.marker       = mb0;
                r.type_hash    = 0;
                offsetAt       = markerAt + 1;
            }
            (void) two_byte_offset;  // offset form is also implied by
                                     // the offset byte's LSB below
                                     // (kept loaded above for the body-
                                     // length disambiguation).
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
                // Real Delphi RSMs interleave additional sub-record
                // tags into the proc body BETWEEN params and locals:
                //   0x23  return-value descriptor (one per function)
                //   0x25  enum entries (one per enum value, for any
                //         enum used as a param / local / return type)
                // These can run for hundreds of bytes (a function
                // with a multi-enum return type like FileSave's
                // TSaveFormat hits ~150 bytes of 0x25 records). We
                // don't decode them; we just need to walk past to
                // find the next 0x20 local. The earlier 6-byte
                // re-anchor scan is too small for this; do a wider
                // 1024-byte forward scan when we hit an unknown
                // tag, looking for the next valid sub-record header
                // (0x20 / 0x21 / 0x22 whose parseSub validates) or
                // the proc-end marker (0x63).
                if (tag != kRecordTagParam && tag != kRecordTagVariable
                    && tag != kRecordTagVarParam) {
                    const std::size_t bigScan = std::min(
                        s + 1024,
                        buf.size() > 8 ? buf.size() - 8 : 0);
                    std::size_t ns = s + 1;
                    bool found = false;
                    while (ns < bigScan) {
                        const auto t2 = static_cast<std::uint8_t>(buf[ns]);
                        if (t2 == kFunctionEndMarker) {
                            found = true;
                            break;
                        }
                        if ((t2 == kRecordTagParam
                             || t2 == kRecordTagVariable
                             || t2 == kRecordTagVarParam)
                            && parseSub(ns).ok) {
                            found = true;
                            break;
                        }
                        ++ns;
                    }
                    if (!found) break;
                    s = ns;
                    continue;
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
}

} // namespace rsm2pdb::rsm
