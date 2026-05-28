#pragma once

// Tiny header-only helpers shared by the cli/ subcommand
// implementations and the top-level dispatch in main.cpp.

#include <algorithm>
#include <cctype>
#include <string>

namespace rsm2pdb::cli {

// Lowercase file extension (without the dot), e.g.
// "C:/x/y.RSM" -> "rsm". Returns "" when the path has no '.'
// component.
inline std::string extLower(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e;
}

} // namespace rsm2pdb::cli
