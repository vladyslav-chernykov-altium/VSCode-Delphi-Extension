#pragma once

#include "model/model.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace rsm2pdb::map {

// Raw parsed view of a Delphi .map file (DCC_MapFile=3 output).
// Format: ASCII, line-oriented, multiple labelled sections.

struct Segment {
    std::uint16_t id;          // 1, 2, ...
    std::uint64_t start_va;    // image-base-relative segment start
    std::uint64_t length;
    std::string   name;        // ".text", ".data", ".bss", ".tls", ".pdata"
    std::string   klass;       // "CODE" / "DATA" / "BSS" / "TLS" / "PDATA"
};

struct ModuleInSegment {
    std::uint16_t segment_id;
    std::uint64_t segment_offset;   // offset relative to segment start
    std::uint64_t length;
    std::string   klass;
    std::string   section;          // ".text", ...
    std::string   group;            // "(none)" or "DGROUP"
    std::string   module_name;      // "System", "hello", "Winapi.Windows"
    std::uint32_t alignment;
};

struct Public {
    std::uint16_t segment_id;
    std::uint64_t segment_offset;
    std::string   name;             // "hello.Add", "System.SysUtils.AnsiPos", ...
};

struct LineRecord {
    std::uint32_t line;
    std::uint16_t segment_id;
    std::uint64_t segment_offset;
};

struct LineTable {
    std::string module_name;        // "hello"
    std::string source_path;        // "hello.dpr"
    std::string segment_name;       // ".text"
    std::vector<LineRecord> lines;
};

struct EntryPoint {
    bool          has_value = false;
    std::uint16_t segment_id = 0;
    std::uint64_t segment_offset = 0;
};

struct MapFile {
    std::vector<Segment>         segments;
    std::vector<ModuleInSegment> module_segments;
    std::vector<Public>          publics;
    std::vector<LineTable>       line_tables;
    EntryPoint                   entry_point;

    // Helpers
    const Segment* findSegment(std::uint16_t id) const;
};

class Reader {
public:
    // Returns true on success; on failure populates error.
    bool open(const std::string& path);

    const MapFile& file() const { return file_; }
    const std::string& error() const { return error_; }

    void dump(std::FILE* out) const;

private:
    bool parse(const std::string& text);

    MapFile     file_;
    std::string error_;
};

// Convert parsed MapFile data into the debugger-agnostic model::Module.
void populate(const MapFile& mf, model::Module& mod);

} // namespace rsm2pdb::map
