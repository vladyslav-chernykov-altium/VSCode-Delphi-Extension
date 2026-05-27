// Tests for the tiny Delphi-x64 prologue disassembler. Patterns are
// taken directly from `cdb -c "uf locals!locals.<name>"` runs against
// examples/04_locals -- see the RE notes in todo.txt / docs.

#include "doctest/doctest.h"

#include "pe/prologue.h"

#include <cstdint>
#include <vector>

using rsm2pdb::pe::parsePrologueSubRsp;

TEST_CASE("parsePrologueSubRsp: classic procedure shape (sub rsp, 0x30)") {
    // GlobalProc2 prologue:
    //   55             push rbp
    //   48 83 EC 30    sub  rsp, 30h
    //   48 8B EC       mov  rbp, rsp
    //   89 4D 40       mov  [rbp+40h], ecx
    const std::vector<std::uint8_t> code = {
        0x55, 0x48, 0x83, 0xEC, 0x30,
        0x48, 0x8B, 0xEC,
        0x89, 0x4D, 0x40,
    };
    CHECK(parsePrologueSubRsp(code.data(), code.size()) == 0x30);
}

TEST_CASE("parsePrologueSubRsp: function shape (sub rsp, 0x10)") {
    // GlobalFunc1 prologue.
    const std::vector<std::uint8_t> code = {
        0x55, 0x48, 0x83, 0xEC, 0x10,
        0x48, 0x8B, 0xEC,
    };
    CHECK(parsePrologueSubRsp(code.data(), code.size()) == 0x10);
}

TEST_CASE("parsePrologueSubRsp: large frame via imm32 (sub rsp, 0x200)") {
    // 48 81 EC 00 02 00 00 -- sub rsp, 0x200.
    const std::vector<std::uint8_t> code = {
        0x55, 0x48, 0x81, 0xEC, 0x00, 0x02, 0x00, 0x00,
        0x48, 0x8B, 0xEC,
    };
    CHECK(parsePrologueSubRsp(code.data(), code.size()) == 0x200);
}

TEST_CASE("parsePrologueSubRsp: imm8 form is sign-extended") {
    // Hypothetical: 48 83 EC 80 = sub rsp, -128 (would never happen
    // in practice but the byte-level decode must still respect i8).
    const std::vector<std::uint8_t> code = {
        0x55, 0x48, 0x83, 0xEC, 0x80,
    };
    CHECK(parsePrologueSubRsp(code.data(), code.size()) == -128);
}

TEST_CASE("parsePrologueSubRsp: frameless thunk returns 0") {
    // No `push rbp` -- e.g. tiny tail-call thunk like:
    //   48 FF 25 ... jmp [rip + offset]
    const std::vector<std::uint8_t> code = {
        0x48, 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK(parsePrologueSubRsp(code.data(), code.size()) == 0);
}

TEST_CASE("parsePrologueSubRsp: push rbp but no sub returns 0") {
    // Leaf method may skip the local-area carve-out entirely.
    const std::vector<std::uint8_t> code = {
        0x55, 0x48, 0x8B, 0xEC,           // push rbp; mov rbp, rsp
        0xB8, 0x00, 0x00, 0x00, 0x00,     // mov eax, 0
    };
    CHECK(parsePrologueSubRsp(code.data(), code.size()) == 0);
}

TEST_CASE("parsePrologueSubRsp: null and short buffers are safe") {
    CHECK(parsePrologueSubRsp(nullptr, 0) == 0);
    CHECK(parsePrologueSubRsp(nullptr, 100) == 0);
    const std::uint8_t short_buf[] = { 0x55, 0x48 };
    CHECK(parsePrologueSubRsp(short_buf, sizeof(short_buf)) == 0);

    // imm32 form needs 8 bytes; only 7 available -> bail out.
    const std::uint8_t imm32_truncated[] = {
        0x55, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00,
    };
    CHECK(parsePrologueSubRsp(imm32_truncated,
                              sizeof(imm32_truncated)) == 0);
}
