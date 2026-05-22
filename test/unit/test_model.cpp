#include <doctest/doctest.h>

#include "model/model.h"

using namespace rsm2pdb::model;

TEST_CASE("Module assigns 1-based type ids") {
    Module m;
    auto id1 = m.addType({PrimitiveType{PrimitiveKind::Int32}});
    auto id2 = m.addType({PrimitiveType{PrimitiveKind::Float64}});
    CHECK(id1 == 1);
    CHECK(id2 == 2);
}

TEST_CASE("Module roundtrips type kinds") {
    Module m;
    auto id = m.addType({PrimitiveType{PrimitiveKind::Bool}});
    const auto& t = m.getType(id);
    const auto* p = std::get_if<PrimitiveType>(&t.kind);
    REQUIRE(p != nullptr);
    CHECK(p->kind == PrimitiveKind::Bool);
}
