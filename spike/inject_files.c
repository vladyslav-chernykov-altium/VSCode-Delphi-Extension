/*
 * inject_files.c  -  PoC (proof of concept) injector for rsm2pdb.
 *
 * Like spike/inject_dwarf.c, but reads .debug_* section bytes from
 * FILES (as produced by `rsm2pdb dwarf-emit`) rather than from
 * another PE. This wires together our map -> model -> dwarf pipeline
 * end-to-end so gdb can step Delphi code through the resulting PE.
 *
 * Usage:
 *   inject_files <input.exe> <dwarf-dir> <output.exe>
 *
 * <dwarf-dir> must contain at least:
 *     .debug_info
 *     .debug_abbrev
 *     .debug_line
 *   Empty files are tolerated and skipped.
 *
 * Throwaway PoC; production injector (step 15) will be C++ + more
 * robust (header-area expansion, error reporting, no global state).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INJECT 8

typedef struct {
    char name[32];
    uint8_t *data;
    uint32_t size;
    uint32_t strtab_offset;
} DebugSection;

static const char *kSectionNames[] = {
    ".debug_abbrev",
    ".debug_str",
    ".debug_line_str",
    ".debug_line",
    ".debug_info",
    NULL
};

static uint8_t *slurp_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "short read on %s\n", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int write_file(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s for write\n", path); return -1; }
    int ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok ? 0 : -1;
}

static uint32_t round_up(uint32_t v, uint32_t align) {
    return (v + align - 1) & ~(align - 1);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
            "usage: inject_files <input.exe> <dwarf-dir> <output.exe>\n");
        return 2;
    }
    const char *exe_in   = argv[1];
    const char *dwarf_dir= argv[2];
    const char *exe_out  = argv[3];

    /* ---- Load DWARF section bytes from files ---- */
    DebugSection sections[MAX_INJECT];
    int n_sections = 0;
    for (int i = 0; kSectionNames[i]; ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dwarf_dir, kSectionNames[i]);
        size_t sz = 0;
        uint8_t *bytes = slurp_file(path, &sz);
        if (!bytes) {
            fprintf(stderr, "warn: skipping %s (could not read)\n", path);
            continue;
        }
        if (sz == 0) {
            fprintf(stderr, "info: %s is empty - skipping\n",
                    kSectionNames[i]);
            free(bytes);
            continue;
        }
        strncpy(sections[n_sections].name,
                kSectionNames[i],
                sizeof(sections[n_sections].name) - 1);
        sections[n_sections].name[sizeof(sections[n_sections].name) - 1] = 0;
        sections[n_sections].data = bytes;
        sections[n_sections].size = (uint32_t)sz;
        printf("source %-15s %u bytes\n",
               kSectionNames[i], (unsigned)sz);
        ++n_sections;
    }
    if (n_sections == 0) {
        fprintf(stderr, "error: no DWARF section files found in %s\n",
                dwarf_dir);
        return 1;
    }

    /* ---- Load target PE ---- */
    size_t tgt_size = 0;
    uint8_t *tgt = slurp_file(exe_in, &tgt_size);
    if (!tgt) return 1;

    PIMAGE_DOS_HEADER tgt_dos = (PIMAGE_DOS_HEADER)tgt;
    PIMAGE_NT_HEADERS64 tgt_nt =
        (PIMAGE_NT_HEADERS64)(tgt + tgt_dos->e_lfanew);

    if (tgt_nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        fprintf(stderr, "error: target is not x64 PE\n");
        return 1;
    }

    PIMAGE_SECTION_HEADER tgt_secs = IMAGE_FIRST_SECTION(tgt_nt);
    uint16_t tgt_n = tgt_nt->FileHeader.NumberOfSections;

    uint32_t file_align = tgt_nt->OptionalHeader.FileAlignment;
    uint32_t sect_align = tgt_nt->OptionalHeader.SectionAlignment;
    uint32_t size_of_headers = tgt_nt->OptionalHeader.SizeOfHeaders;

    /* Header room check; expand SizeOfHeaders if necessary by
     * shifting all section data DOWN by one (or more) FileAlignment
     * unit. Virtual addresses are unaffected. */
    uint32_t sect_table_end_off =
        (uint32_t)((uint8_t *)(tgt_secs + tgt_n) - tgt);
    uint32_t header_free = size_of_headers - sect_table_end_off;
    uint32_t header_need =
        (uint32_t)n_sections * sizeof(IMAGE_SECTION_HEADER);
    uint32_t header_expansion = 0;
    if (header_need > header_free) {
        header_expansion =
            round_up(header_need - header_free, file_align);
        printf("header area: free=%u, need=%u -> expand by %u bytes\n",
               header_free, header_need, header_expansion);
    } else {
        printf("header area: free=%u, need=%u (no expansion needed)\n",
               header_free, header_need);
    }

    /* ---- Existing symbol + string table (preserve and extend) ---- */
    uint32_t old_symtab_offset = tgt_nt->FileHeader.PointerToSymbolTable;
    uint32_t old_n_symbols    = tgt_nt->FileHeader.NumberOfSymbols;
    uint32_t old_symtab_size  = old_n_symbols * 18;
    uint32_t old_strtab_size  = 0;
    if (old_symtab_offset != 0) {
        memcpy(&old_strtab_size,
               tgt + old_symtab_offset + old_symtab_size, 4);
    } else {
        old_strtab_size = 4;
    }
    printf("target symtab: off=0x%x n_syms=%u strtab_size=%u\n",
           old_symtab_offset, old_n_symbols, old_strtab_size);

    /* Build new string table = old + our long section names */
    uint32_t new_strtab_size = old_strtab_size;
    for (int i = 0; i < n_sections; ++i) {
        sections[i].strtab_offset = new_strtab_size;
        new_strtab_size += (uint32_t)strlen(sections[i].name) + 1;
    }
    uint8_t *new_strtab = (uint8_t *)calloc(1, new_strtab_size);
    if (!new_strtab) { fprintf(stderr, "OOM\n"); return 1; }
    if (old_symtab_offset != 0) {
        memcpy(new_strtab,
               tgt + old_symtab_offset + old_symtab_size,
               old_strtab_size);
    } else {
        memcpy(new_strtab, &old_strtab_size, 4);
    }
    memcpy(new_strtab, &new_strtab_size, 4);  /* patch new size */
    for (int i = 0; i < n_sections; ++i) {
        memcpy(new_strtab + sections[i].strtab_offset,
               sections[i].name,
               strlen(sections[i].name) + 1);
    }

    /* ---- Layout ----
     * If we expand headers, every existing section's PointerToRawData
     * (file offset) is shifted by header_expansion. VAs are unchanged.
     */
    uint32_t cur_va = 0, cur_fo = 0;
    for (uint16_t i = 0; i < tgt_n; ++i) {
        uint32_t v_end = tgt_secs[i].VirtualAddress +
                         (tgt_secs[i].Misc.VirtualSize >
                          tgt_secs[i].SizeOfRawData
                              ? tgt_secs[i].Misc.VirtualSize
                              : tgt_secs[i].SizeOfRawData);
        if (v_end > cur_va) cur_va = v_end;
        uint32_t f_end = tgt_secs[i].PointerToRawData + header_expansion +
                         tgt_secs[i].SizeOfRawData;
        if (f_end > cur_fo) cur_fo = f_end;
    }
    if (old_symtab_offset != 0) {
        uint32_t f_end = old_symtab_offset + header_expansion +
                         old_symtab_size + old_strtab_size;
        if (f_end > cur_fo) cur_fo = f_end;
    }
    if ((uint32_t)tgt_size + header_expansion > cur_fo) {
        cur_fo = (uint32_t)tgt_size + header_expansion;
    }
    cur_va = round_up(cur_va, sect_align);
    cur_fo = round_up(cur_fo, file_align);

    uint32_t section_data_start = cur_fo;
    uint32_t section_data_end = cur_fo;
    for (int i = 0; i < n_sections; ++i) {
        section_data_end += round_up(sections[i].size, file_align);
    }
    uint32_t final_symtab_offset = section_data_end;
    uint32_t final_size = section_data_end + old_symtab_size + new_strtab_size;

    uint8_t *out = (uint8_t *)calloc(1, final_size);
    if (!out) { fprintf(stderr, "OOM\n"); return 1; }

    if (header_expansion == 0) {
        memcpy(out, tgt, tgt_size);
    } else {
        /* Copy headers as-is (we'll patch PointerToRawData and
         * SizeOfHeaders afterwards), then copy section data shifted
         * by header_expansion. */
        memcpy(out, tgt, size_of_headers);
        memcpy(out + size_of_headers + header_expansion,
               tgt + size_of_headers,
               tgt_size - size_of_headers);
    }

    PIMAGE_NT_HEADERS64 out_nt =
        (PIMAGE_NT_HEADERS64)(out + tgt_dos->e_lfanew);
    PIMAGE_SECTION_HEADER out_secs = IMAGE_FIRST_SECTION(out_nt);

    /* Shift existing sections' file offsets by header_expansion. */
    if (header_expansion != 0) {
        for (uint16_t i = 0; i < tgt_n; ++i) {
            if (out_secs[i].PointerToRawData != 0) {
                out_secs[i].PointerToRawData += header_expansion;
            }
        }
        out_nt->OptionalHeader.SizeOfHeaders =
            size_of_headers + header_expansion;
        /* PointerToSymbolTable too if it's in the old area. */
        if (out_nt->FileHeader.PointerToSymbolTable != 0) {
            out_nt->FileHeader.PointerToSymbolTable += header_expansion;
        }
    }

    /* Recompute existing symbol-table offset after shift, to put new
     * symtab after the shifted data. (Drop the now-moved old symtab
     * data location; we'll re-place it at the very end.) */
    uint32_t shifted_old_symtab_offset =
        old_symtab_offset == 0 ? 0 : old_symtab_offset + header_expansion;

    /* Copy old symbol entries + write new string table */
    if (shifted_old_symtab_offset != 0 && old_symtab_size > 0) {
        memcpy(out + final_symtab_offset,
               out + shifted_old_symtab_offset,
               old_symtab_size);
    }
    memcpy(out + final_symtab_offset + old_symtab_size,
           new_strtab, new_strtab_size);

    /* Write section data + new section headers */
    cur_fo = section_data_start;
    for (int i = 0; i < n_sections; ++i) {
        PIMAGE_SECTION_HEADER nh = &out_secs[tgt_n + i];
        memset(nh, 0, sizeof(*nh));

        char namefield[9];
        snprintf(namefield, sizeof(namefield),
                 "/%u", sections[i].strtab_offset);
        memset(nh->Name, 0, 8);
        memcpy(nh->Name, namefield, strlen(namefield));

        nh->Misc.VirtualSize  = sections[i].size;
        nh->VirtualAddress    = cur_va;
        nh->SizeOfRawData     = round_up(sections[i].size, file_align);
        nh->PointerToRawData  = cur_fo;
        nh->Characteristics   =
            IMAGE_SCN_MEM_READ |
            IMAGE_SCN_CNT_INITIALIZED_DATA |
            IMAGE_SCN_MEM_DISCARDABLE;

        printf("inject %-15s name='%s' VA=%08x FO=%08x size=%u\n",
               sections[i].name, namefield,
               cur_va, cur_fo, sections[i].size);

        memcpy(out + cur_fo, sections[i].data, sections[i].size);
        cur_va = round_up(cur_va + sections[i].size, sect_align);
        cur_fo = round_up(cur_fo + sections[i].size, file_align);
    }

    /* Patch headers */
    out_nt->FileHeader.NumberOfSections      = tgt_n + (uint16_t)n_sections;
    out_nt->FileHeader.PointerToSymbolTable  = final_symtab_offset;
    out_nt->OptionalHeader.SizeOfImage       = cur_va;
    out_nt->OptionalHeader.CheckSum          = 0;

    printf("output: NumberOfSections=%u SizeOfImage=0x%x final_file=%u\n",
           out_nt->FileHeader.NumberOfSections,
           out_nt->OptionalHeader.SizeOfImage,
           final_size);

    if (write_file(exe_out, out, final_size) != 0) return 1;
    printf("wrote %s\n", exe_out);

    free(tgt); free(out); free(new_strtab);
    for (int i = 0; i < n_sections; ++i) free(sections[i].data);
    return 0;
}
