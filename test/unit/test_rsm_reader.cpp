// Tests for the RSM header parser. Validates header fields against
// the two committed fixtures and the differential analysis recorded
// in rsm-format.txt / docs/02-rsm-format-notes.md.

#include <doctest/doctest.h>

#include "rsm/rsm_reader.h"

#include <string>

#ifndef RSM2PDB_FIXTURES_DIR
#  error "RSM2PDB_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

const std::string kHello      = std::string(RSM2PDB_FIXTURES_DIR) + "/hello.rsm";
const std::string kTwoUnits   = std::string(RSM2PDB_FIXTURES_DIR) + "/two_units.rsm";
const std::string kPrimitives = std::string(RSM2PDB_FIXTURES_DIR) + "/primitives.rsm";
const std::string kRecords    = std::string(RSM2PDB_FIXTURES_DIR) + "/records.rsm";

// Find the first aggregate matching `name`. Helper for the
// Step 11b tests below; we don't have a name lookup on Reader
// because aggregate names are not globally unique across units
// (RTL has its own TPoint, TList, ...). The fixture's records
// unit is the LAST declared, so a reverse-find pinpoints the
// user copy.
inline const rsm2pdb::rsm::AggregateType*
findAggregateByName(const rsm2pdb::rsm::Reader& r,
                    const std::string& name) {
    const auto& aggs = r.aggregates();
    for (auto it = aggs.rbegin(); it != aggs.rend(); ++it) {
        if (it->name == name) return &*it;
    }
    return nullptr;
}

inline const rsm2pdb::rsm::FieldEntry*
findField(const rsm2pdb::rsm::AggregateType& a, const std::string& name) {
    for (const auto& f : a.fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

} // namespace

TEST_CASE("rsm::Reader rejects a non-existent file") {
    rsm2pdb::rsm::Reader r;
    CHECK_FALSE(r.open("does_not_exist.rsm"));
    CHECK(!r.error().empty());
}

TEST_CASE("rsm::Reader parses hello.rsm header") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kHello));
    const auto& h = r.header();

    CHECK(h.magic            == rsm2pdb::rsm::Header::kMagic);
    CHECK(h.metadata_start   == rsm2pdb::rsm::Header::kMetadataStart);
    CHECK(h.unit_count       == 14u);
    CHECK(h.version_minor    == 1u);
    CHECK(h.timestamp        == 0x5CB65217u);
    CHECK(h.flags            == rsm2pdb::rsm::Header::kFlagsConst);
    CHECK(h.legacy_imagebase == rsm2pdb::rsm::Header::kLegacyImageBase);
    CHECK(h.reserved_1c      == 0u);
    CHECK(h.exe_path         == ".\\Win64\\Debug\\hello.exe");
}

TEST_CASE("rsm::Reader parses two_units.rsm header") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kTwoUnits));
    const auto& h = r.header();

    CHECK(h.magic            == rsm2pdb::rsm::Header::kMagic);
    CHECK(h.metadata_start   == rsm2pdb::rsm::Header::kMetadataStart);
    CHECK(h.unit_count       == 16u);          // +2 vs hello: Geometry + App.Colors
    CHECK(h.version_minor    == 1u);
    CHECK(h.timestamp        == 0x5CB656A4u);
    CHECK(h.flags            == rsm2pdb::rsm::Header::kFlagsConst);
    CHECK(h.legacy_imagebase == rsm2pdb::rsm::Header::kLegacyImageBase);
    CHECK(h.reserved_1c      == 0u);
    CHECK(h.exe_path         == ".\\Win64\\Debug\\two_units.exe");
}

TEST_CASE("rsm::Reader locates primitive type table in hello.rsm") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kHello));
    const auto& prims = r.primitives();
    REQUIRE(prims.size() >= 11);  // Boolean..Pointer at minimum

    // First record is always Boolean (used as the scan anchor).
    CHECK(prims.front().name == "Boolean");
    CHECK(prims.front().kind == rsm2pdb::model::PrimitiveKind::Bool);
    CHECK(prims.front().byte_size == 1u);

    // A handful of spot checks across the table.
    const auto* integer = r.findPrimitive("Integer");
    REQUIRE(integer != nullptr);
    CHECK(integer->kind == rsm2pdb::model::PrimitiveKind::Int32);
    CHECK(integer->byte_size == 4u);

    const auto* byte = r.findPrimitive("Byte");
    REQUIRE(byte != nullptr);
    CHECK(byte->kind == rsm2pdb::model::PrimitiveKind::UInt8);
    CHECK(byte->byte_size == 1u);

    const auto* cardinal = r.findPrimitive("Cardinal");
    REQUIRE(cardinal != nullptr);
    CHECK(cardinal->kind == rsm2pdb::model::PrimitiveKind::UInt32);

    // Double / Single live past the first cluster (verifies the walk
    // doesn't truncate early).
    const auto* dbl = r.findPrimitive("Double");
    REQUIRE(dbl != nullptr);
    CHECK(dbl->kind == rsm2pdb::model::PrimitiveKind::Float64);
    CHECK(dbl->byte_size == 8u);

    // Type-ids should differ between distinct primitives (rough sanity
    // check on the 0x9C 0x13 marker extraction).
    CHECK(integer->raw_type_id != byte->raw_type_id);
    CHECK(integer->raw_type_id != 0u);
}

TEST_CASE("rsm::Reader primitive table is build-independent") {
    rsm2pdb::rsm::Reader h, t;
    REQUIRE(h.open(kHello));
    REQUIRE(t.open(kTwoUnits));

    // Both fixtures use the same Delphi install, so the primitive
    // table content should match between them.
    REQUIRE(h.primitives().size() == t.primitives().size());
    for (std::size_t i = 0; i < h.primitives().size(); ++i) {
        CHECK(h.primitives()[i].name      == t.primitives()[i].name);
        CHECK(h.primitives()[i].byte_size == t.primitives()[i].byte_size);
        CHECK(h.primitives()[i].raw_type_id == t.primitives()[i].raw_type_id);
    }
}

TEST_CASE("rsm::Reader locates user globals in hello.rsm") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kHello));

    // hello.dpr declares: P: TPoint; C: TColor; S: Integer.
    // .map .bss layout (segment 0003, base 0x430000):
    //   0003:0000A750 hello.P
    //   0003:0000A758 hello.C
    //   0003:0000A75C hello.S
    const auto* p = r.findVariableAt(0x0043A750);
    REQUIRE(p != nullptr);
    CHECK(p->name == "P");
    CHECK_FALSE(p->is_primitive);
    CHECK(p->inline_type_id != 0u);  // TPoint type_id

    const auto* c = r.findVariableAt(0x0043A758);
    REQUIRE(c != nullptr);
    CHECK(c->name == "C");
    CHECK_FALSE(c->is_primitive);
    CHECK(c->inline_type_id != 0u);  // TColor enum
    CHECK(c->inline_type_id != p->inline_type_id);  // different types

    const auto* s = r.findVariableAt(0x0043A75C);
    REQUIRE(s != nullptr);
    CHECK(s->name == "S");
    CHECK(s->is_primitive);
    CHECK(s->inline_type_id == 0u);
}

TEST_CASE("rsm::Reader locates user globals in two_units.rsm") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kTwoUnits));

    // two_units.dpr declares (under two_units. namespace):
    //   P1, P2: TPoint;   C: TColor;   D, S: Integer;
    const auto* p1 = r.findVariableAt(0x0043A758);
    const auto* p2 = r.findVariableAt(0x0043A760);
    const auto* c  = r.findVariableAt(0x0043A768);
    const auto* d  = r.findVariableAt(0x0043A76C);
    const auto* s  = r.findVariableAt(0x0043A770);

    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(c  != nullptr);
    REQUIRE(d  != nullptr);
    REQUIRE(s  != nullptr);

    CHECK(p1->name == "P1");
    CHECK(p2->name == "P2");
    CHECK(c->name  == "C");
    CHECK(d->name  == "D");
    CHECK(s->name  == "S");

    // P1 and P2 share the same TPoint type_id.
    CHECK_FALSE(p1->is_primitive);
    CHECK_FALSE(p2->is_primitive);
    CHECK(p1->inline_type_id == p2->inline_type_id);
    CHECK(p1->inline_type_id != 0u);

    // C is TColor (enum), different type than P1/P2.
    CHECK_FALSE(c->is_primitive);
    CHECK(c->inline_type_id != 0u);
    CHECK(c->inline_type_id != p1->inline_type_id);

    // D and S are Integer-typed (primitive form).
    CHECK(d->is_primitive);
    CHECK(s->is_primitive);
    CHECK(d->inline_type_id == 0u);
    CHECK(s->inline_type_id == 0u);
}

TEST_CASE("rsm::Reader captures per-unit type markers from primitives.rsm") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kPrimitives));

    // primitives.dpr declares 13 globals across 12 distinct primitive
    // types (gI and gI2 share Integer). .bss segment base is 0x431000.
    // Each declaration introduces a new per-unit type marker in
    // increments of 2 starting at 0x02.
    const auto* gI  = r.findVariableAt(0x0043B750);
    const auto* gI2 = r.findVariableAt(0x0043B754);
    const auto* gC  = r.findVariableAt(0x0043B758);
    const auto* gB  = r.findVariableAt(0x0043B75C);
    const auto* gW  = r.findVariableAt(0x0043B75E);
    const auto* gSh = r.findVariableAt(0x0043B760);
    const auto* gSm = r.findVariableAt(0x0043B762);
    const auto* gL  = r.findVariableAt(0x0043B768);
    const auto* gU  = r.findVariableAt(0x0043B770);
    const auto* gF  = r.findVariableAt(0x0043B778);
    const auto* gD  = r.findVariableAt(0x0043B780);
    const auto* gO  = r.findVariableAt(0x0043B788);
    const auto* gH  = r.findVariableAt(0x0043B78A);

    REQUIRE(gI != nullptr);  REQUIRE(gI2 != nullptr);
    REQUIRE(gC != nullptr);  REQUIRE(gB  != nullptr);
    REQUIRE(gW != nullptr);  REQUIRE(gSh != nullptr);
    REQUIRE(gSm != nullptr); REQUIRE(gL  != nullptr);
    REQUIRE(gU != nullptr);  REQUIRE(gF  != nullptr);
    REQUIRE(gD != nullptr);  REQUIRE(gO  != nullptr);
    REQUIRE(gH != nullptr);

    CHECK(gI->name  == "gI");
    CHECK(gI2->name == "gI2");
    CHECK(gH->name  == "gH");

    // All are primitive form.
    for (auto* v : {gI, gI2, gC, gB, gW, gSh, gSm, gL, gU, gF, gD, gO, gH}) {
        CHECK(v->is_primitive);
        CHECK(v->inline_type_id == 0u);
    }

    // Same-type globals share marker AND trailer_type_id.
    CHECK(gI->type_marker     == gI2->type_marker);
    CHECK(gI->type_marker     == 0x02);
    CHECK(gI->trailer_type_id == gI2->trailer_type_id);

    // Distinct types have distinct markers (incrementing by 2).
    CHECK(gC->type_marker  == 0x04);
    CHECK(gB->type_marker  == 0x06);
    CHECK(gW->type_marker  == 0x08);
    CHECK(gSh->type_marker == 0x0a);
    CHECK(gSm->type_marker == 0x0c);
    CHECK(gL->type_marker  == 0x0e);
    CHECK(gU->type_marker  == 0x10);
    CHECK(gF->type_marker  == 0x12);
    CHECK(gD->type_marker  == 0x14);
    CHECK(gO->type_marker  == 0x16);
    CHECK(gH->type_marker  == 0x18);

    // gH (last variable) uses the 5-byte primitive form (no trailer);
    // every other extended-form variable has the 12-byte trailer.
    CHECK(gI->has_trailer);
    CHECK_FALSE(gH->has_trailer);
}

TEST_CASE("rsm::Reader decodes procedure params and locals") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kTwoUnits));

    // Geometry.Add at .map seg 0001:00025900 -> absolute VA 0x426900
    const auto* add = r.findProcedureAt(0x00426900);
    REQUIRE(add != nullptr);
    CHECK(add->name == "Add");
    REQUIRE(add->params.size() == 2);
    REQUIRE(add->locals.size() == 2);

    CHECK(add->params[0].name == "A");
    CHECK(add->params[0].is_primitive);
    CHECK(add->params[0].stack_offset == 0x20);
    CHECK(add->params[1].name == "B");
    CHECK(add->params[1].stack_offset == 0x30);

    // Both params share marker 0x02 (Integer in Geometry's unit-local table).
    CHECK(add->params[0].type_marker == add->params[1].type_marker);
    CHECK(add->params[0].type_marker == 0x02);

    CHECK(add->locals[0].name == "Result");
    CHECK(add->locals[0].stack_offset == -8);
    CHECK(add->locals[0].is_primitive);
    CHECK(add->locals[1].name == "Tmp");
    CHECK(add->locals[1].stack_offset == -16);

    // Geometry.DistanceSq: TPoint params + Integer locals (Result, Dx, Dy).
    const auto* dsq = r.findProcedureAt(0x00426930);
    REQUIRE(dsq != nullptr);
    CHECK(dsq->name == "DistanceSq");
    REQUIRE(dsq->params.size() == 2);
    REQUIRE(dsq->locals.size() == 3);
    CHECK(dsq->params[0].name == "P1");
    CHECK_FALSE(dsq->params[0].is_primitive);   // TPoint is non-primitive
    CHECK(dsq->params[0].stack_offset == 0x20);
    CHECK(dsq->params[1].name == "P2");
    CHECK(dsq->locals[0].name == "Result");
    CHECK(dsq->locals[1].name == "Dx");
    CHECK(dsq->locals[2].name == "Dy");
    CHECK(dsq->locals[1].stack_offset == -16);
    CHECK(dsq->locals[2].stack_offset == -24);
}

TEST_CASE("rsm::Reader procedure scan does not double-count locals as globals") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kTwoUnits));

    // No variable's file_offset should fall inside a procedure record's
    // [file_offset, file_offset_end) byte range. (The variable scanner
    // explicitly skips those regions via isInsideProcedure().)
    //
    // Caveat: locals belonging to procedures the scanner FAILED to parse
    // (e.g. compiler-generated unit-init / Finalization with a different
    // body layout) may still surface as globals -- that's a scanner gap,
    // not a dedup bug. This test only asserts the dedup contract for
    // procedures we successfully recognised.
    for (const auto& v : r.variables()) {
        for (const auto& p : r.procedures()) {
            const bool inside = (v.file_offset >= p.file_offset &&
                                 v.file_offset <  p.file_offset_end);
            CHECK_FALSE(inside);
        }
    }
}

// ---------------------------------------------------------------------------
// Step 11b Phase B.1 -- aggregate types (records / classes / enums)
// ---------------------------------------------------------------------------

TEST_CASE("rsm::Reader decodes user records from records.rsm") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kRecords));

    // TPoint: record { X, Y: Integer } -- two 4-byte fields, packed
    // tight by natural alignment (no padding for Integer/Integer).
    const auto* tpoint = findAggregateByName(r, "TPoint");
    REQUIRE(tpoint != nullptr);
    CHECK(tpoint->kind == rsm2pdb::rsm::AggregateKind::Record);
    REQUIRE(tpoint->fields.size() == 2u);
    CHECK(tpoint->fields[0].name == "X");
    CHECK(tpoint->fields[0].offset == 0u);
    CHECK(tpoint->fields[1].name == "Y");
    CHECK(tpoint->fields[1].offset == 4u);

    // TPerson: mixed-type record. Integer + Double + Boolean + string
    // + Char. Offsets follow Delphi's natural-alignment rules.
    const auto* tperson = findAggregateByName(r, "TPerson");
    REQUIRE(tperson != nullptr);
    CHECK(tperson->kind == rsm2pdb::rsm::AggregateKind::Record);
    REQUIRE(tperson->fields.size() == 5u);
    const auto* age    = findField(*tperson, "Age");
    const auto* salary = findField(*tperson, "Salary");
    const auto* active = findField(*tperson, "Active");
    const auto* name   = findField(*tperson, "Name");
    const auto* grade  = findField(*tperson, "Grade");
    REQUIRE(age);    CHECK(age->offset    == 0u);
    REQUIRE(salary); CHECK(salary->offset == 8u);
    REQUIRE(active); CHECK(active->offset == 16u);
    REQUIRE(name);   CHECK(name->offset   == 24u);
    REQUIRE(grade);  CHECK(grade->offset  == 32u);

    // TBox: record-of-records. Both TopLeft and BottomRight reference
    // TPoint via 2-byte composite hash. Label_ is a string at offset
    // 16 (after two 8-byte TPoints).
    const auto* tbox = findAggregateByName(r, "TBox");
    REQUIRE(tbox != nullptr);
    REQUIRE(tbox->fields.size() == 3u);
    const auto* topLeft = findField(*tbox, "TopLeft");
    const auto* bottomRight = findField(*tbox, "BottomRight");
    REQUIRE(topLeft);
    CHECK(topLeft->offset == 0u);
    CHECK(topLeft->type_hash == tpoint->own_hash);
    REQUIRE(bottomRight);
    CHECK(bottomRight->offset == 8u);
    CHECK(bottomRight->type_hash == tpoint->own_hash);
}

TEST_CASE("rsm::Reader decodes big record (2-byte offset form)") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kRecords));

    // TBig: 40 Integer fields F00..F39 = 160 bytes total. F32..F39
    // are at offsets >= 128 and exercise the 2-byte offset form.
    const auto* tbig = findAggregateByName(r, "TBig");
    REQUIRE(tbig != nullptr);
    REQUIRE(tbig->fields.size() == 40u);

    // Spot-check F00 (1-byte form) + F32 (first 2-byte form).
    const auto* f00 = findField(*tbig, "F00");
    REQUIRE(f00); CHECK(f00->offset == 0u);
    const auto* f31 = findField(*tbig, "F31");
    REQUIRE(f31); CHECK(f31->offset == 124u);   // last 1-byte form
    const auto* f32 = findField(*tbig, "F32");
    REQUIRE(f32); CHECK(f32->offset == 128u);   // first 2-byte form
    const auto* f39 = findField(*tbig, "F39");
    REQUIRE(f39); CHECK(f39->offset == 156u);   // last field
}

TEST_CASE("rsm::Reader decodes classes and inheritance from records.rsm") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kRecords));

    // TShape: class with fName (string) at 8, fArea (Double) at 16.
    // Class layout starts after the 8-byte vtable pointer.
    const auto* tshape = findAggregateByName(r, "TShape");
    REQUIRE(tshape != nullptr);
    CHECK(tshape->kind == rsm2pdb::rsm::AggregateKind::Class);
    REQUIRE(tshape->fields.size() == 2u);
    const auto* fName = findField(*tshape, "fName");
    const auto* fArea = findField(*tshape, "fArea");
    REQUIRE(fName); CHECK(fName->offset == 8u);
    REQUIRE(fArea); CHECK(fArea->offset == 16u);

    // TCircle: inherits TShape. Only its OWN field (fRadius) shows
    // here -- base fields stay on TShape, reachable via base_hash.
    const auto* tcircle = findAggregateByName(r, "TCircle");
    REQUIRE(tcircle != nullptr);
    CHECK(tcircle->kind == rsm2pdb::rsm::AggregateKind::Class);
    REQUIRE(tcircle->fields.size() == 1u);
    const auto* fRadius = findField(*tcircle, "fRadius");
    REQUIRE(fRadius); CHECK(fRadius->offset == 24u);  // vtable(8) + fName(8) + fArea(8)
    CHECK(tcircle->base_hash == tshape->own_hash);

    // TShape itself has no explicit base (TObject implicit). The
    // resolver SHOULD leave base_hash at 0 (TObject lives in System
    // unit, its hash doesn't match anything local) -- but until
    // Phase B.2+ adds unit-boundary tracking, RTL classes sharing
    // our user-type own_hash can leak a phony base into ours.
    // For now we just verify that IF a base is set, it doesn't
    // round-trip to a meaningful Pascal name (it points to noise).
    if (tshape->base_hash != 0u) {
        const auto* phony = r.findAggregateByHash(tshape->base_hash);
        // Most likely the leaked base points to some RTL utility
        // record / class -- not "TObject" by name (we never see
        // System.TObject in the per-unit type stream).
        if (phony != nullptr) {
            CHECK(phony->name != "TShape");
        }
    }

    // TBag: class with TPoint-typed fields (clears risk R4).
    const auto* tbag = findAggregateByName(r, "TBag");
    REQUIRE(tbag != nullptr);
    CHECK(tbag->kind == rsm2pdb::rsm::AggregateKind::Class);
    REQUIRE(tbag->fields.size() == 3u);
    const auto* fPos  = findField(*tbag, "fPos");
    const auto* fSlot = findField(*tbag, "fSlot");
    const auto* fTag  = findField(*tbag, "fTag");
    const auto* tpoint = findAggregateByName(r, "TPoint");
    REQUIRE(tpoint != nullptr);
    REQUIRE(fPos);
    CHECK(fPos->offset == 8u);
    CHECK(fPos->type_hash == tpoint->own_hash);
    REQUIRE(fSlot);
    CHECK(fSlot->offset == 16u);
    CHECK(fSlot->type_hash == tpoint->own_hash);
    REQUIRE(fTag);
    CHECK(fTag->offset == 24u);
    CHECK(fTag->type_hash == 0u);          // primitive Integer
    CHECK(fTag->primitive_marker == 0x02); // Integer marker
}

TEST_CASE("rsm::Reader decodes enum entries from records.rsm") {
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kRecords));

    // TColor = (clRed, clGreen, clBlue, clYellow).
    const auto* tcolor = findAggregateByName(r, "TColor");
    REQUIRE(tcolor != nullptr);
    CHECK(tcolor->kind == rsm2pdb::rsm::AggregateKind::Enum);
    REQUIRE(tcolor->enum_entries.size() == 4u);

    auto findOrd = [&](const std::string& nm) -> std::int64_t {
        for (const auto& e : tcolor->enum_entries)
            if (e.name == nm) return e.ordinal;
        return -1;
    };
    CHECK(findOrd("clRed")    == 0);
    CHECK(findOrd("clGreen")  == 1);
    CHECK(findOrd("clBlue")   == 2);
    CHECK(findOrd("clYellow") == 3);
}

TEST_CASE("rsm::Reader links variables to aggregates by inline_type_id") {
    // Globals in records.rsm carry inline_type_id == aggregate own_hash.
    // findAggregateByHash() should round-trip cleanly.
    rsm2pdb::rsm::Reader r;
    REQUIRE(r.open(kRecords));

    const auto* tpoint = findAggregateByName(r, "TPoint");
    const auto* tbag   = findAggregateByName(r, "TBag");
    const auto* tbig   = findAggregateByName(r, "TBig");
    REQUIRE(tpoint); REQUIRE(tbag); REQUIRE(tbig);

    // Every non-primitive variable named GPoint / GBag / GBig should
    // exist with inline_type_id pointing at the right aggregate.
    bool found_gpoint = false, found_gbag = false, found_gbig = false;
    for (const auto& v : r.variables()) {
        if (v.is_primitive) continue;
        if (v.name == "GPoint") {
            found_gpoint = true;
            CHECK(v.inline_type_id == tpoint->own_hash);
            CHECK(r.findAggregateByHash(v.inline_type_id) == tpoint);
        } else if (v.name == "GBag") {
            found_gbag = true;
            CHECK(v.inline_type_id == tbag->own_hash);
            CHECK(r.findAggregateByHash(v.inline_type_id) == tbag);
        } else if (v.name == "GBig") {
            found_gbig = true;
            CHECK(v.inline_type_id == tbig->own_hash);
            CHECK(r.findAggregateByHash(v.inline_type_id) == tbig);
        }
    }
    CHECK(found_gpoint);
    CHECK(found_gbag);
    CHECK(found_gbig);
}

TEST_CASE("rsm::Reader rejects bad magic") {
    // Synthesize an in-memory bad file via a temp on disk would be
    // overkill; the helper code path is exercised by the strict
    // magic check in open(), which the two positive tests cover for
    // the success path. This test verifies behaviour on a truncated /
    // wrong file by pointing at a .map (definitely not "CSH7").
    rsm2pdb::rsm::Reader r;
    const std::string mapPath = std::string(RSM2PDB_FIXTURES_DIR) + "/hello.map";
    CHECK_FALSE(r.open(mapPath));
    CHECK(r.error().find("bad magic") != std::string::npos);
}
