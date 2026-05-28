#pragma once

// Resolve a Delphi source filename (typically just a basename like
// "Geometry.pas") against a list of candidate directories. Used by
// the PDB / DWARF subcommands to fill out the absolute paths that
// debuggers need for breakpoint binding (cppvsdbg is especially
// strict and won't bind BPs on relative paths even with
// sourceFileMap).

#include <filesystem>
#include <string>
#include <vector>

namespace rsm2pdb::cli {

inline std::string resolveSourcePath(
    const std::string& raw,
    const std::string& map_path,
    const std::vector<std::string>& extra_dirs) {
    namespace fs = std::filesystem;
    if (raw.empty()) return raw;
    fs::path p(raw);
    if (p.is_absolute() && fs::exists(p)) return p.string();

    // Build the search list: explicit --src-search dirs first, then
    // common Delphi project-layout fallbacks relative to the .map.
    std::vector<fs::path> candidates;
    for (const auto& d : extra_dirs) candidates.emplace_back(d);
    const fs::path map_dir = fs::path(map_path).parent_path();
    candidates.push_back(map_dir);                            // alongside .map
    candidates.push_back(map_dir.parent_path());              // one level up
    candidates.push_back(map_dir.parent_path().parent_path()); // two up

    const std::string base = p.filename().string();
    for (const auto& dir : candidates) {
        fs::path cand = dir / base;
        std::error_code ec;
        if (fs::exists(cand, ec)) return fs::canonical(cand, ec).string();
    }
    return raw;
}

} // namespace rsm2pdb::cli
