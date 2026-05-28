#pragma once

// Tiny disassembler for the head of a Delphi-x64 function body. We
// need two facts to turn RSM-stored stack offsets into rbp-relative
// addresses the debugger expects:
//
//   - sub_rsp        the immediate from `sub rsp, N` (= size of the
//                    local area; rbp ends up at the bottom);
//   - extra_pushes   how many additional callee-saved registers were
//                    pushed between `push rbp` and `sub rsp` (each
//                    one shifts the rcx-shadow / param-spill area up
//                    by 8 bytes).
//
// Empirical Delphi-x64 prologue shapes (validated on examples/
// 04_locals + 05_types):
//
//   simple (most procs):
//     55                       push rbp
//     48 83 EC NN              sub  rsp, imm8
//     48 8B EC / 48 89 E5      mov  rbp, rsp
//
//   large frame (sub > 0x7F):
//     55                       push rbp
//     48 81 EC NN NN NN NN     sub  rsp, imm32
//     48 8B EC                 mov  rbp, rsp
//
//   with callee-saved pushes (ProbeLocals etc.):
//     55                       push rbp
//     57                       push rdi             \
//     56                       push rsi              | extra_pushes
//     41 54                    push r12 (REX.B)     /
//     48 81 EC NN NN NN NN     sub  rsp, imm32
//     48 8B EC                 mov  rbp, rsp
//
// Notably this is NOT the canonical MS-x64 sequence (`mov rbp, rsp`
// BEFORE `sub rsp, N`); Delphi parks rbp at the BOTTOM of the local
// area, so every spilled param/local sits at a POSITIVE offset from
// rbp. The `sub rsp` immediate counts the local-area bytes; the
// extra-push count counts the bytes between the local area top and
// the saved-rbp slot (=> rcx-shadow lives at rbp + sub_rsp +
// 8*(extra_pushes + 1) + 8 = rbp + sub_rsp + 16 + 8*extra_pushes).

#include <cstddef>
#include <cstdint>

namespace rsm2pdb::pe {

struct ParsedPrologue {
    // Immediate from `sub rsp, imm` (0 = pattern didn't match;
    // callers fall back to raw RSM/2 offsets in that case).
    std::int32_t  sub_rsp = 0;
    // Number of `push <reg>` instructions seen between `push rbp`
    // and `sub rsp`. Each adds 8 to the shadow-store base offset.
    std::uint8_t  extra_pushes = 0;
};

inline ParsedPrologue parsePrologue(const std::uint8_t* code,
                                    std::size_t len) {
    ParsedPrologue out{};
    if (!code || len < 5) return out;
    if (code[0] != 0x55) return out;             // push rbp

    std::size_t i = 1;
    // Skip a run of single-byte push <reg> (RAX..RDI) and
    // two-byte push <r8..r15> (REX.B + push <reg>). Limit to 16
    // to avoid runaway on a malformed buffer.
    std::uint8_t extras = 0;
    while (i < len && extras < 16) {
        const auto b0 = code[i];
        if (b0 >= 0x50 && b0 <= 0x57) {
            ++i; ++extras;
            continue;
        }
        if (b0 == 0x41 && i + 1 < len) {
            const auto b1 = code[i + 1];
            if (b1 >= 0x50 && b1 <= 0x57) {
                i += 2; ++extras;
                continue;
            }
        }
        break;
    }
    out.extra_pushes = extras;

    // sub rsp encoded as REX.W + 83/81 EC <imm>; we need bytes
    // i..i+3 (imm8) or i..i+6 (imm32) -- bounds-check accordingly.
    if (i + 3 >= len) return out;
    if (code[i] != 0x48) return out;             // REX.W

    if (code[i + 1] == 0x83 && code[i + 2] == 0xEC) {
        // 48 83 EC NN -- sub rsp, imm8.
        out.sub_rsp = static_cast<std::int32_t>(
                          static_cast<std::int8_t>(code[i + 3]));
        return out;
    }
    if (code[i + 1] == 0x81 && code[i + 2] == 0xEC) {
        // 48 81 EC NN NN NN NN -- sub rsp, imm32 (LE).
        if (i + 6 >= len) return out;
        const auto v = static_cast<std::uint32_t>(code[i + 3])
                     | (static_cast<std::uint32_t>(code[i + 4]) << 8)
                     | (static_cast<std::uint32_t>(code[i + 5]) << 16)
                     | (static_cast<std::uint32_t>(code[i + 6]) << 24);
        out.sub_rsp = static_cast<std::int32_t>(v);
        return out;
    }
    return out;
}

// Back-compat wrapper. Existing callers that only care about the
// frame size can keep using the int-returning form.
inline std::int32_t parsePrologueSubRsp(const std::uint8_t* code,
                                        std::size_t len) {
    return parsePrologue(code, len).sub_rsp;
}

} // namespace rsm2pdb::pe
