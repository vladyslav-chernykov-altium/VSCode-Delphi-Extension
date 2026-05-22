#pragma once

#include <cstdio>
#include <string>

namespace rsm2pdb::rsm {

class Reader {
public:
    bool open(const std::string& path);
    void dump(std::FILE* out) const;

private:
    std::string path_;
};

} // namespace rsm2pdb::rsm
