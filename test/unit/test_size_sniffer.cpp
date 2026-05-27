// Tests for the rbp-relative width sniffer. Byte sequences below are
// taken directly from `cdb uf locals!locals.<name>` against
// examples/04_locals -- see the RE notes in todo.txt.

#include "doctest/doctest.h"

#include "pe/size_sniffer.h"

#include <cstdint>
#include <vector>

using rsm2pdb::pe::sniffStackVarSizes;

TEST_CASE("sniffStackVarSizes: GlobalProc2 prologue spills 2 Integer params") {
    // 48 83 EC 30 | 48 8B EC                       prologue
    // 89 4D 40                                     mov [rbp+40h], ecx  (pA, 4)
    // 89 55 48                                     mov [rbp+48h], edx  (pB, 4)
    // 8B 45 40                                     mov eax, [rbp+40h]
    // 89 45 2C                                     mov [rbp+2Ch], eax  (lA, 4)
    const std::vector<std::uint8_t> code = {
        0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xEC,
        0x89, 0x4D, 0x40,
        0x89, 0x55, 0x48,
        0x8B, 0x45, 0x40,
        0x89, 0x45, 0x2C,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(0x40) == 4u);
    CHECK(m.at(0x48) == 4u);
    CHECK(m.at(0x2C) == 4u);
}

TEST_CASE("sniffStackVarSizes: REX.W spill of qword Self (TBase.RegProc1)") {
    // 48 89 4D 40   mov qword [rbp+40h], rcx  (Self, 8)
    // 89 55 48      mov dword [rbp+48h], edx  (pA, 4)
    const std::vector<std::uint8_t> code = {
        0x48, 0x89, 0x4D, 0x40,
        0x89, 0x55, 0x48,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(0x40) == 8u);
    CHECK(m.at(0x48) == 4u);
}

TEST_CASE("sniffStackVarSizes: managed-string init via qword imm32 + spill") {
    // 48 C7 45 28 00 00 00 00   mov qword [rbp+28h], 0       (lS init, 8)
    // 48 89 4D 40               mov qword [rbp+40h], rcx     (pS, 8)
    const std::vector<std::uint8_t> code = {
        0x48, 0xC7, 0x45, 0x28, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x4D, 0x40,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(0x28) == 8u);
    CHECK(m.at(0x40) == 8u);
}

TEST_CASE("sniffStackVarSizes: 16-bit operand via 0x66 prefix") {
    // 66 89 4D 20   mov word [rbp+20h], cx  (2)
    const std::vector<std::uint8_t> code = {
        0x66, 0x89, 0x4D, 0x20,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(0x20) == 2u);
}

TEST_CASE("sniffStackVarSizes: byte access via 0x88 / 0xC6") {
    // 88 4D 10      mov byte [rbp+10h], cl    (1)
    // C6 45 18 7F   mov byte [rbp+18h], 0x7F  (1)
    const std::vector<std::uint8_t> code = {
        0x88, 0x4D, 0x10,
        0xC6, 0x45, 0x18, 0x7F,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(0x10) == 1u);
    CHECK(m.at(0x18) == 1u);
}

TEST_CASE("sniffStackVarSizes: disp32 ModR/M form (GlobalProc10 high args)") {
    // 8B 85 80 00 00 00   mov eax, dword [rbp+0x80]  (4)
    // 8B 85 88 00 00 00   mov eax, dword [rbp+0x88]  (4)
    const std::vector<std::uint8_t> code = {
        0x8B, 0x85, 0x80, 0x00, 0x00, 0x00,
        0x8B, 0x85, 0x88, 0x00, 0x00, 0x00,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(0x80) == 4u);
    CHECK(m.at(0x88) == 4u);
}

TEST_CASE("sniffStackVarSizes: first write wins -- prologue width sticks") {
    // 48 89 4D 40   mov qword [rbp+40h], rcx  (8 -- spill comes first)
    // 8B 45 40      mov eax,   dword [rbp+40h] (would be 4 but ignored)
    const std::vector<std::uint8_t> code = {
        0x48, 0x89, 0x4D, 0x40,
        0x8B, 0x45, 0x40,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(0x40) == 8u);
}

TEST_CASE("sniffStackVarSizes: rbp-relative access with negative disp") {
    // 89 45 F8   mov dword [rbp-8], eax  (disp8 = -8)
    const std::vector<std::uint8_t> code = {
        0x89, 0x45, 0xF8,
    };
    const auto m = sniffStackVarSizes(code.data(), code.size());
    CHECK(m.at(-8) == 4u);
}

TEST_CASE("sniffStackVarSizes: null + short buffers safe") {
    CHECK(sniffStackVarSizes(nullptr, 0).empty());
    CHECK(sniffStackVarSizes(nullptr, 100).empty());

    // Truncated mid-disp32 -- must not record anything spurious.
    const std::uint8_t truncated[] = { 0x89, 0x85, 0x00, 0x01 };
    CHECK(sniffStackVarSizes(truncated, sizeof(truncated)).empty());
}

TEST_CASE("sniffStackVarSizes: instructions that don't touch [rbp+disp] are skipped") {
    // 48 89 D8   mov rax, rbx  (reg-reg, no [rbp])
    // 89 C8      mov eax, ecx
    const std::vector<std::uint8_t> code = {
        0x48, 0x89, 0xD8,
        0x89, 0xC8,
    };
    CHECK(sniffStackVarSizes(code.data(), code.size()).empty());
}
