#pragma once

// Tiny disassembler for the head of a Delphi-x64 function body. We
// only care about the size of the local-area `sub rsp, N` so the PDB
// emitter can translate RSM-stored stack offsets (which are encoded
// relative to "bottom of locals") into rbp-relative offsets the
// debugger expects.
//
// Empirical Delphi-x64 prologue (validated on examples/04_locals):
//   55                       push rbp
//   48 83 EC NN              sub  rsp, imm8        (typical, 8-bit imm)
//   48 81 EC NN NN NN NN     sub  rsp, imm32       (large frames)
//   48 8B EC / 48 89 E5      mov  rbp, rsp
//
// Notably this is NOT the canonical MS-x64 sequence (`mov rbp, rsp`
// BEFORE `sub rsp, N`); Delphi parks rbp at the BOTTOM of the local
// area, so every spilled param/local sits at a positive offset from
// rbp. The `sub rsp` immediate is exactly the byte count between
// rbp and the first shadow slot (= where rcx-shadow lives).

#include <cstddef>
#include <cstdint>

namespace rsm2pdb::pe {

// Returns the immediate from `sub rsp, imm` in the function's
// prologue, or 0 if the prologue doesn't match the expected pattern
// (frameless thunks, tail-call-only stubs, etc.). 0 is a useful
// fallback because applying the offset formula with sub_rsp=0 yields
// raw "RSM/2" values -- not correct, but visible in the debugger, so
// the user can spot the misdecode rather than seeing nothing.
inline std::int32_t parsePrologueSubRsp(const std::uint8_t* code,
                                        std::size_t len) {
    if (!code || len < 5) return 0;
    if (code[0] != 0x55) return 0;              // push rbp
    if (code[1] != 0x48) return 0;              // REX.W

    if (code[2] == 0x83 && code[3] == 0xEC) {
        // 48 83 EC NN -- sub rsp, imm8 (sign-extended; always positive
        // in Delphi-emitted prologues but be explicit anyway).
        return static_cast<std::int32_t>(
                   static_cast<std::int8_t>(code[4]));
    }
    if (code[2] == 0x81 && code[3] == 0xEC) {
        // 48 81 EC NN NN NN NN -- sub rsp, imm32 (LE).
        if (len < 8) return 0;
        const auto v = static_cast<std::uint32_t>(code[4])
                     | (static_cast<std::uint32_t>(code[5]) << 8)
                     | (static_cast<std::uint32_t>(code[6]) << 16)
                     | (static_cast<std::uint32_t>(code[7]) << 24);
        return static_cast<std::int32_t>(v);
    }
    return 0;
}

} // namespace rsm2pdb::pe
