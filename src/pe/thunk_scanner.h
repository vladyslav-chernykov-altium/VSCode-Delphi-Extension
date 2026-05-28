#pragma once

// Delphi-x64 interface adjuster-thunk detector.
//
// When a class implements an interface, Delphi emits per-method
// adjuster thunks that convert an interface pointer back to an
// object pointer (by subtracting the interface field's offset from
// `Self` in rcx) and tail-call the real method. The thunks live as
// raw bytes between class metadata and user code -- they have no
// .map entry, no line info, and no S_PROC32 in the PDB, so when
// step-into lands at one cppvsdbg's "step into" silently degrades
// to "step over" (the engine sees no debug info at the target and
// jumps to next-line-in-caller). That makes Step Into on an
// interface method call appear to skip the method body entirely.
//
// Empirically (examples/06_interface/iface.exe at @0x00426d5a):
//
//     48 83 c1 e0                 add rcx, imm8 (sign-extended)
//     e9 NN NN NN NN              jmp rel32
//
// Total 9 bytes, byte-aligned, consecutive thunks pack tight (no
// padding). Adjustment is always negative (the interface field
// sits N bytes inside the object), typical values -0x20 / -0x18
// for first-implementing-class layouts. rel32 is sign-extended,
// can point forward (to a method later in .text) or backward (to a
// RTL ancestor like TInterfacedObject._AddRef in the same image).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rsm2pdb::pe {

// One detected thunk.
struct AdjusterThunk {
    std::uint64_t va;            // thunk's first byte VA (= PC at `add rcx, ...`)
    std::uint64_t target_va;     // resolved jmp target VA
    std::int32_t  adjustment;    // sign-extended imm8 from `add rcx, NN`
    std::uint16_t segment;       // 1-based PE section index containing the thunk
    std::uint32_t section_off;   // offset within that section
    static constexpr std::uint32_t kSize = 9;  // every thunk is exactly 9 bytes
};

// Scan the PE's executable bytes for `48 83 c1 NN e9 NN NN NN NN`
// patterns. For each hit, resolve the jmp target VA and return it.
// The caller decides which target VAs count as "real methods" and
// which to filter (e.g. drop targets that don't resolve to a known
// public symbol).
//
//   pe_bytes      -- the mapped PE image bytes (whole file).
//   sections      -- COFF section headers from the PE; we only scan
//                    sections whose Characteristics include the
//                    IMAGE_SCN_CNT_CODE (0x20) bit set.
//   image_base    -- PE's OptionalHeader.ImageBase; needed to turn
//                    section RVAs into absolute VAs.
//
// Returns all thunks found, in ascending VA order. Caller can
// post-filter by checking target_va against the known-method list.
inline std::vector<AdjusterThunk> scanAdjusterThunks(
    const std::vector<std::uint8_t>& pe_bytes,
    const std::vector<std::pair<std::uint32_t /*va*/,
                                std::uint32_t /*size*/>>& code_sections,
    /*per-section*/
    const std::vector<std::pair<std::uint32_t /*file_off*/,
                                std::uint16_t /*seg_idx (1-based)*/>>&
            section_file_offs,
    std::uint64_t image_base) {
    std::vector<AdjusterThunk> out;
    out.reserve(64);

    for (std::size_t si = 0; si < code_sections.size(); ++si) {
        const std::uint32_t sec_va   = code_sections[si].first;
        const std::uint32_t sec_size = code_sections[si].second;
        const std::uint32_t file_off = section_file_offs[si].first;
        const std::uint16_t seg_idx  = section_file_offs[si].second;
        if (sec_size < AdjusterThunk::kSize) continue;
        if (static_cast<std::size_t>(file_off) + sec_size > pe_bytes.size())
            continue;

        const std::uint8_t* p = pe_bytes.data() + file_off;
        // Walk every byte position; thunks aren't always aligned in
        // an obvious way and we only need to keep the work cheap.
        // O(N) over the .text section.
        for (std::uint32_t i = 0;
             i + AdjusterThunk::kSize <= sec_size;
             ++i) {
            if (p[i]     != 0x48) continue;     // REX.W
            if (p[i + 1] != 0x83) continue;     // sub group, imm8 form
            if (p[i + 2] != 0xC1) continue;     // mod/rm: add r/m64, imm8
                                                //         (reg = 0 = ADD,
                                                //          rm  = 1 = RCX)
            if (p[i + 4] != 0xE9) continue;     // jmp rel32
            const std::int32_t adj =
                static_cast<std::int32_t>(static_cast<std::int8_t>(p[i + 3]));
            const std::uint32_t rel =
                  static_cast<std::uint32_t>(p[i + 5])
                | (static_cast<std::uint32_t>(p[i + 6]) <<  8)
                | (static_cast<std::uint32_t>(p[i + 7]) << 16)
                | (static_cast<std::uint32_t>(p[i + 8]) << 24);
            const std::int32_t srel = static_cast<std::int32_t>(rel);

            // Sanity: only accept negative adjustments in [-0x80,0]
            // (positive Self offsets don't happen in Delphi's layout
            // and a +Self thunk would be either dead code or noise).
            if (adj > 0 || adj < -0x70) continue;

            const std::uint64_t thunk_va = image_base + sec_va + i;
            // PC after the jmp = start + 9.
            const std::uint64_t next_va = thunk_va + AdjusterThunk::kSize;
            const std::uint64_t target  =
                static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(next_va) +
                    static_cast<std::int64_t>(srel));

            AdjusterThunk t{};
            t.va          = thunk_va;
            t.target_va   = target;
            t.adjustment  = adj;
            t.segment     = seg_idx;
            t.section_off = i;
            out.push_back(t);
            // Skip the 9 bytes we just consumed -- consecutive
            // thunks are tight-packed so the next thunk starts here.
            i += AdjusterThunk::kSize - 1;
        }
    }
    return out;
}

} // namespace rsm2pdb::pe
