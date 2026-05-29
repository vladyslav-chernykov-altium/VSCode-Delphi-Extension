#include "rsm/rsm_reader.h"
#include "rsm/rsm_internal.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

namespace rsm2pdb::rsm {

using namespace detail;

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

    scanPrimitiveTable(buf);

    scanProceduresAndVariables(buf);
    scanPerUnitTypeTables(buf);

    scanAggregateTypes(buf);

    resolveAggregateTypes();

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
    const auto* e = unitTypeEntryForMarker(unit_anchor_offset, marker);
    return e ? &e->name : nullptr;
}

const Reader::UnitTypeEntry* Reader::unitTypeEntryForMarker(
        std::uint64_t unit_anchor_offset, std::uint8_t marker) const {
    if (unit_anchor_offset == 0) return nullptr;
    if (marker == 0 || (marker & 0x01) != 0) return nullptr;
    auto it = primary_table_by_anchor_.find(unit_anchor_offset);
    if (it == primary_table_by_anchor_.end()) return nullptr;
    const auto idx = static_cast<std::size_t>(marker) / 2;
    if (idx == 0 || idx > it->second.size()) return nullptr;
    return &it->second[idx - 1];
}

} // namespace rsm2pdb::rsm
