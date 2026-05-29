#include "rsm/rsm_reader.h"
#include "rsm/rsm_internal.h"

#include <cstdint>
#include <cstdio>

namespace rsm2pdb::rsm {

using namespace detail;

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

} // namespace rsm2pdb::rsm
