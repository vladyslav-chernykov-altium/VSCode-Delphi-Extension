#pragma once

// Tiny x86-64 instruction scanner that infers the byte-width of each
// stack-variable in a Delphi-compiled function by looking at the
// MOVs Delphi emits to spill/load the variable. Covers the subset of
// the instruction set Delphi-x64 uses in practice:
//
//   88 /r ib            mov  [rbp+disp8],  r8
//   8A /r ib            mov  r8,           [rbp+disp8]
//   C6 /r ib ib         mov  byte [rbp+disp8], imm8
//   89 /r ib            mov  [rbp+disp8],  r32
//   8B /r ib            mov  r32,          [rbp+disp8]
//   C7 /r ib id         mov  dword [rbp+disp8], imm32
//   66 89 /r ib         mov  [rbp+disp8],  r16
//   66 8B /r ib         mov  r16,          [rbp+disp8]
//   48 89 /r ib         mov  [rbp+disp8],  r64
//   48 8B /r ib         mov  r64,          [rbp+disp8]
//   48 C7 /r ib id      mov  qword [rbp+disp8], imm32 (sign-extended)
//
// Each form also has a disp32 ModR/M variant (mod=10).
//
// We never need to fully tracewise-decode every instruction -- we just
// recognise these specific shapes wherever they appear in the body
// and record (disp, width) into a map. First write wins, since
// Delphi's spill prologue is the first thing to touch each slot and
// it tells us the parameter's "natural" width.
//
// False positives are possible (random byte sequences mid-instruction
// can look like a MOV) but rare in tightly-codegen'd Delphi output --
// good enough as a fallback when the RSM marker table can't resolve a
// variable's size.

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace rsm2pdb::pe {

// Walk `code[0..len)` and return offset -> byte-width for every
// rbp-relative memory access we recognise. Map key is the signed
// displacement from rbp; value is 1/2/4/8.
inline std::unordered_map<std::int32_t, std::uint32_t>
sniffStackVarSizes(const std::uint8_t* code, std::size_t len) {
    std::unordered_map<std::int32_t, std::uint32_t> out;
    if (!code) return out;

    auto record = [&](std::int32_t disp, std::uint32_t width) {
        // First write wins -- Delphi's prologue spills ECX/EDX/R8D/R9D
        // first, which is exactly the access width we want to surface.
        out.emplace(disp, width);
    };

    // For ModR/M with mod=01 (8-bit displacement, rm=101 = RBP):
    //   ModR/M = 01 reg 101  ->  bits & 0xC7 == 0x45
    // For mod=10 (32-bit displacement, rm=101):
    //   ModR/M = 10 reg 101  ->  bits & 0xC7 == 0x85
    auto isRbpDisp8  = [](std::uint8_t mr) { return (mr & 0xC7) == 0x45; };
    auto isRbpDisp32 = [](std::uint8_t mr) { return (mr & 0xC7) == 0x85; };

    for (std::size_t i = 0; i < len; ++i) {
        std::size_t pos = i;
        bool op16 = false;       // 0x66 operand-size override
        bool rex_w = false;       // REX.W -> 64-bit operand

        if (pos < len && code[pos] == 0x66) { op16 = true; ++pos; }
        if (pos < len && (code[pos] & 0xF0) == 0x40) {
            if (code[pos] & 0x08) rex_w = true;
            ++pos;
        }
        if (pos >= len) break;

        const std::uint8_t op = code[pos++];
        std::uint32_t width = 0;
        std::uint32_t imm_after = 0;   // additional immediate bytes
                                       // following the displacement.

        switch (op) {
            case 0x88:  // mov [r/m8], r8
            case 0x8A:  // mov r8, [r/m8]
                width = 1; break;
            case 0xC6:  // mov [r/m8], imm8
                width = 1; imm_after = 1; break;
            case 0x89:  // mov [r/m], r
            case 0x8B:  // mov r, [r/m]
                width = rex_w ? 8 : (op16 ? 2 : 4); break;
            case 0xC7:  // mov [r/m], imm32  (imm16 with 0x66; sign-ext
                        // imm32 with REX.W -> qword).
                width = rex_w ? 8 : (op16 ? 2 : 4);
                imm_after = op16 ? 2 : 4;
                break;
            default:
                continue;   // not an instruction shape we care about
        }
        if (pos >= len) continue;

        const std::uint8_t mr = code[pos++];
        if (isRbpDisp8(mr)) {
            if (pos >= len) continue;
            const auto disp = static_cast<std::int32_t>(
                static_cast<std::int8_t>(code[pos]));
            ++pos;
            if (pos + imm_after > len) continue;
            record(disp, width);
            i = pos + imm_after - 1;   // -1 because for() will ++ it.
        } else if (isRbpDisp32(mr)) {
            if (pos + 4 > len) continue;
            const auto disp = static_cast<std::int32_t>(
                  (static_cast<std::uint32_t>(code[pos    ]))
                | (static_cast<std::uint32_t>(code[pos + 1]) << 8)
                | (static_cast<std::uint32_t>(code[pos + 2]) << 16)
                | (static_cast<std::uint32_t>(code[pos + 3]) << 24));
            pos += 4;
            if (pos + imm_after > len) continue;
            record(disp, width);
            i = pos + imm_after - 1;
        }
        // Other ModR/M shapes -- skip; let the outer loop try the
        // next byte as a potential instruction start.
    }
    return out;
}

} // namespace rsm2pdb::pe
