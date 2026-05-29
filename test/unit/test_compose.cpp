// Tests for the compose::resolveFunction frame-resolver. These don't
// touch a real RSM file; they construct ProcedureRecord / Variable
// fixtures by hand and verify the rbp-offset formula + size-resolution
// chain produce the addresses we observed empirically in cdb-disasm
// of examples/05_types ProbeLocals + examples/04_locals GlobalProc2.

#include "doctest/doctest.h"

#include "compose/frame.h"

#include <cstdint>
#include <vector>

using rsm2pdb::compose::buildMarkerSizes;
using rsm2pdb::compose::isSelf;
using rsm2pdb::compose::resolveFunction;
using rsm2pdb::rsm::ProcedureRecord;
using rsm2pdb::rsm::Variable;

namespace {

// Helper: synthesise a primitive Variable for proc.params / .locals.
Variable mkVar(const char* name, std::int32_t stack_offset,
               std::uint8_t marker = 0x02,
               const char* pascal = "") {
    Variable v{};
    v.name         = name;
    v.stack_offset = stack_offset;
    v.type_marker  = marker;
    v.is_primitive = true;
    v.pascal_type  = pascal;
    return v;
}

// Helper: build a GlobalProc2-like prologue (simple shape).
//   push rbp; sub rsp, 0x30; mov rbp, rsp
const std::vector<std::uint8_t> kProlog_Simple_0x30 = {
    0x55, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xEC,
    // Token spill so the size sniffer has something to chew on
    // even though our tests don't rely on it.
    0x89, 0x4D, 0x40,   // mov [rbp+0x40], ecx
};

// ProbeLocals-like multi-push prologue:
//   push rbp; push rdi; push rsi; sub rsp, 0x1C0; mov rbp, rsp
const std::vector<std::uint8_t> kProlog_MultiPush_0x1C0 = {
    0x55, 0x57, 0x56,
    0x48, 0x81, 0xEC, 0xC0, 0x01, 0x00, 0x00,
    0x48, 0x8B, 0xEC,
};

} // namespace

TEST_CASE("compose::resolveFunction: simple prologue locals (GlobalProc2)") {
    // Mirror examples/04_locals GlobalProc2: 2 Integer params at
    // RSM +32/+48, 2 Integer locals at RSM -8/-16.
    ProcedureRecord proc{};
    proc.name = "GlobalProc2";
    proc.params.push_back(mkVar("pA", 32));
    proc.params.push_back(mkVar("pB", 48));
    proc.locals.push_back(mkVar("lA", -8));
    proc.locals.push_back(mkVar("lB", -16));

    const auto rf = resolveFunction(
        proc,
        kProlog_Simple_0x30.data(),
        kProlog_Simple_0x30.size(),
        {});
    CHECK(rf.sub_rsp == 0x30);
    CHECK(rf.extra_pushes == 0u);
    REQUIRE(rf.vars.size() == 4);
    CHECK(rf.vars[0].name == "pA");
    CHECK(rf.vars[0].rbp_offset == 0x40);   // sub_rsp + 16 + 0
    CHECK(rf.vars[1].name == "pB");
    CHECK(rf.vars[1].rbp_offset == 0x48);   // sub_rsp + 24 + 0
    CHECK(rf.vars[2].name == "lA");
    CHECK(rf.vars[2].rbp_offset == 0x2C);   // sub_rsp - 4
    CHECK(rf.vars[3].name == "lB");
    CHECK(rf.vars[3].rbp_offset == 0x28);   // sub_rsp - 8
}

TEST_CASE("compose::resolveFunction: multi-push shifts both params and locals") {
    // 2 extra pushes (rdi, rsi) -> 16-byte shadow shift for params,
    // 16-byte downward shift for locals. Delphi reserves the top of
    // the sub_rsp area for the saved callee-saved register(s) when
    // try/except is in scope, so the effective local area starts
    // lower and the raw -> real translation needs the -param_shift
    // correction. Verified empirically on 08_inherit_props.ProbeAll.
    ProcedureRecord proc{};
    proc.name = "MultiPushProc";
    proc.params.push_back(mkVar("p", 32));
    proc.locals.push_back(mkVar("l", -8));

    const auto rf = resolveFunction(
        proc,
        kProlog_MultiPush_0x1C0.data(),
        kProlog_MultiPush_0x1C0.size(),
        {});
    CHECK(rf.sub_rsp == 0x1C0);
    CHECK(rf.extra_pushes == 2u);
    REQUIRE(rf.vars.size() == 2);
    CHECK(rf.vars[0].name == "p");
    // sub_rsp(0x1C0) + RSM/2(16) + 8*2 (param_shift) = 0x1E0
    CHECK(rf.vars[0].rbp_offset == 0x1E0);
    CHECK(rf.vars[1].name == "l");
    // sub_rsp(0x1C0) - param_shift(16) + (-8/2) = 0x1AC
    CHECK(rf.vars[1].rbp_offset == 0x1AC);
}

TEST_CASE("compose::resolveFunction: Self uses rcx-shadow slot") {
    // Class method with Self marker 0x29 and sentinel +4109 offset.
    // Should bypass the general formula and land at sub_rsp + 16.
    ProcedureRecord proc{};
    proc.name = "TBase.RegProc2";
    auto self = mkVar("Self", 4109, /*marker=*/ 0x29);
    proc.params.push_back(std::move(self));
    proc.params.push_back(mkVar("pA", 48));

    const auto rf = resolveFunction(
        proc,
        kProlog_Simple_0x30.data(),
        kProlog_Simple_0x30.size(),
        {});
    REQUIRE(rf.vars.size() == 2);
    CHECK(rf.vars[0].name == "Self");
    CHECK(rf.vars[0].is_self == true);
    CHECK(rf.vars[0].rbp_offset == 0x40);   // sub_rsp + 16
    CHECK(rf.vars[0].byte_size  == 8);      // forced to pointer width
    CHECK(rf.vars[1].name == "pA");
    CHECK(rf.vars[1].is_self == false);
    CHECK(rf.vars[1].rbp_offset == 0x48);
}

TEST_CASE("compose::resolveFunction: nested function gets __frame_outer__") {
    // RSM marks the function with has_static_link; the resolver
    // synthesises a pointer-sized slot at the rcx-shadow position.
    ProcedureRecord proc{};
    proc.name = "NestedInner";
    proc.has_static_link = true;
    proc.params.push_back(mkVar("pB", 32));
    proc.locals.push_back(mkVar("lInner", -8));

    const auto rf = resolveFunction(
        proc,
        kProlog_Simple_0x30.data(),
        kProlog_Simple_0x30.size(),
        {});
    REQUIRE(rf.vars.size() == 3);
    CHECK(rf.vars[0].name == "__frame_outer__");
    CHECK(rf.vars[0].is_static_link == true);
    CHECK(rf.vars[0].is_param == true);
    CHECK(rf.vars[0].rbp_offset == 0x40);   // sub_rsp + 16
    CHECK(rf.vars[0].byte_size  == 8);
    CHECK(rf.vars[1].name == "pB");
    CHECK(rf.vars[2].name == "lInner");
}

TEST_CASE("compose::resolveFunction: pascal_type overrides size fallback") {
    // pascal_type "Integer" -> Int32 kind + 4-byte size. Wins over
    // marker_sizes (which the test deliberately sets to a wrong width
    // to prove the precedence chain).
    ProcedureRecord proc{};
    proc.locals.push_back(mkVar("lI", -8, /*marker=*/ 0x02, "Integer"));

    std::unordered_map<std::uint8_t, std::uint32_t> marker_sizes;
    marker_sizes[0x02] = 1;  // intentionally wrong

    const auto rf = resolveFunction(
        proc,
        kProlog_Simple_0x30.data(),
        kProlog_Simple_0x30.size(),
        marker_sizes);
    REQUIRE(rf.vars.size() == 1);
    CHECK(rf.vars[0].byte_size == 4);
    REQUIRE(rf.vars[0].prim_kind.has_value());
    CHECK(*rf.vars[0].prim_kind == rsm2pdb::model::PrimitiveKind::Int32);
}

TEST_CASE("compose::resolveFunction: marker_sizes used when pascal_type empty") {
    // No pascal_type -> fall back to marker_sizes.
    ProcedureRecord proc{};
    proc.locals.push_back(mkVar("l", -8, /*marker=*/ 0x04));

    std::unordered_map<std::uint8_t, std::uint32_t> marker_sizes;
    marker_sizes[0x04] = 2;

    const auto rf = resolveFunction(
        proc,
        kProlog_Simple_0x30.data(),
        kProlog_Simple_0x30.size(),
        marker_sizes);
    REQUIRE(rf.vars.size() == 1);
    CHECK(rf.vars[0].byte_size == 2);
    CHECK_FALSE(rf.vars[0].prim_kind.has_value());
}

TEST_CASE("compose::resolveFunction: degrades gracefully when prologue unrecognised") {
    // Frameless thunk: just `ret` / `jmp`. parsePrologue returns 0,
    // resolveFunction degrades to raw RSM/2 offsets without crashing.
    const std::vector<std::uint8_t> frameless = { 0xC3 };
    ProcedureRecord proc{};
    proc.locals.push_back(mkVar("l", -8));

    const auto rf = resolveFunction(
        proc, frameless.data(), frameless.size(), {});
    CHECK(rf.sub_rsp == 0);
    CHECK(rf.extra_pushes == 0u);
    REQUIRE(rf.vars.size() == 1);
    CHECK(rf.vars[0].rbp_offset == -4);
}

TEST_CASE("compose::isSelf catches name + the two non-primitive markers") {
    Variable a; a.name = "Self"; a.type_marker = 0x02;
    CHECK(isSelf(a));

    Variable b; b.name = "x"; b.type_marker = 0x29;
    CHECK(isSelf(b));

    Variable c; c.name = "x"; c.type_marker = 0xD5;
    CHECK(isSelf(c));

    Variable d; d.name = "lA"; d.type_marker = 0x0E;
    CHECK_FALSE(isSelf(d));
}
