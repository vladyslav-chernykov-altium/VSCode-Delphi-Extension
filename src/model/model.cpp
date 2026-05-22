#include "model/model.h"

#include <cassert>

namespace rsm2pdb::model {

TypeId Module::addType(Type t) {
    types_.push_back(std::move(t));
    return static_cast<TypeId>(types_.size());
}

const Type& Module::getType(TypeId id) const {
    assert(id != kNoType && id <= types_.size());
    return types_[id - 1];
}

} // namespace rsm2pdb::model
