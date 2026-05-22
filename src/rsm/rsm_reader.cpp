#include "rsm/rsm_reader.h"

#include <cstdio>

namespace rsm2pdb::rsm {

bool Reader::open(const std::string& path) {
    path_ = path;
    // TODO: open file, read header, validate magic, populate internal structures.
    return true;
}

void Reader::dump(std::FILE* out) const {
    std::fprintf(out, "RSM file: %s\n", path_.c_str());
    std::fprintf(out, "(parser not yet implemented)\n");
}

} // namespace rsm2pdb::rsm
