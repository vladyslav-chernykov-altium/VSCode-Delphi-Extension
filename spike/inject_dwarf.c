/*
 * inject_dwarf.c  -  Throwaway spike (step 6c).
 *
 * Reads .debug_* sections from a "source" PE and appends them as
 * new sections to a copy of a "target" PE. Patches the target's
 * PE headers (NumberOfSections, SizeOfImage, CheckSum=0).
 *
 * Section names like ".debug_line_str" (15 bytes) overflow PE's
 * 8-byte Name field. We follow the COFF convention: the Name field
 * is "/N" where N is a decimal offset into the COFF string table.
 * If the target has no symbol/string table we create one; if it has,
 * we extend the string table with our names.
 *
 * Usage:
 *   inject_dwarf <source.exe> <target.exe> <out.exe>
 *
 * Assumptions for the spike:
 *   - target.exe has at least 5 new section header slots free
 *     between the end of its existing section header table and
 *     SizeOfHeaders. (Verified for our hello_stripped.exe.)
 *   - source.exe is mingw-gcc-produced with .debug_* sections.
 *   - 64-bit PE32+ image only.
 *
 * Production injector (step 14) will handle header-area expansion,
 * 32-bit images, and all DWARF section types.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kWantedSections[] = {
    ".debug_abbrev",
    ".debug_str",
    ".debug_line_str",
    ".debug_line",
    ".debug_info",
    NULL
};

#define MAX_INJECT 8

typedef struct {
    char name[32];              /* full name, null-terminated */
    const uint8_t *data;        /* points into source buffer */
    uint32_t raw_size;
    uint32_t virt_size;
    uint32_t strtab_offset;     /* offset of name in new string table */
} DebugSection;

static uint8_t *slurp_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
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

static int is_wanted(const char *name) {
    for (int i = 0; kWantedSections[i]; ++i) {
        if (strcmp(name, kWantedSections[i]) == 0) return 1;
    }
    return 0;
}

/* Resolve a PE section name (8-byte field, possibly "/N" indirect)
 * into a full name. strtab/strtab_size from the COFF string table.
 */
static void resolve_section_name(const uint8_t name_field[8],
                                 const uint8_t *strtab,
                                 uint32_t strtab_size,
                                 char *out, size_t out_size) {
    if (name_field[0] == '/') {
        char digits[8] = {0};
        memcpy(digits, name_field + 1, 7);
        uint32_t off = (uint32_t)atoi(digits);
        if (strtab && off < strtab_size) {
            strncpy(out, (const char *)strtab + off, out_size - 1);
            out[out_size - 1] = 0;
            return;
        }
    }
    memcpy(out, name_field, 8);
    out[8] = 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
            "usage: inject_dwarf <source.exe> <target.exe> <out.exe>\n");
        return 2;
    }
    const char *src_path = argv[1];
    const char *tgt_path = argv[2];
    const char *out_path = argv[3];

    size_t src_size = 0, tgt_size = 0;
    uint8_t *src = slurp_file(src_path, &src_size);
    uint8_t *tgt = slurp_file(tgt_path, &tgt_size);
    if (!src || !tgt) return 1;

    /* ---- Parse source: find .debug_* sections ---- */

    PIMAGE_DOS_HEADER src_dos = (PIMAGE_DOS_HEADER)src;
    PIMAGE_NT_HEADERS64 src_nt =
        (PIMAGE_NT_HEADERS64)(src + src_dos->e_lfanew);
    PIMAGE_SECTION_HEADER src_secs = IMAGE_FIRST_SECTION(src_nt);
    uint16_t src_n = src_nt->FileHeader.NumberOfSections;

    /* Source's COFF string table for resolving long names */
    const uint8_t *src_strtab = NULL;
    uint32_t src_strtab_size = 0;
    if (src_nt->FileHeader.PointerToSymbolTable != 0) {
        uint32_t off = src_nt->FileHeader.PointerToSymbolTable
                     + src_nt->FileHeader.NumberOfSymbols * 18;
        src_strtab = src + off;
        memcpy(&src_strtab_size, src_strtab, 4);
    }

    DebugSection found[MAX_INJECT];
    int n_found = 0;

    for (uint16_t i = 0; i < src_n; ++i) {
        char name[32];
        resolve_section_name(src_secs[i].Name, src_strtab,
                             src_strtab_size, name, sizeof(name));
        if (!is_wanted(name)) continue;
        if (n_found >= MAX_INJECT) {
            fprintf(stderr, "too many debug sections\n");
            return 1;
        }
        strncpy(found[n_found].name, name, sizeof(found[n_found].name) - 1);
        found[n_found].name[sizeof(found[n_found].name) - 1] = 0;
        found[n_found].data = src + src_secs[i].PointerToRawData;
        found[n_found].raw_size = src_secs[i].SizeOfRawData;
        found[n_found].virt_size = src_secs[i].Misc.VirtualSize;
        printf("source has %-15s vsize=%6u rsize=%6u\n",
               name, found[n_found].virt_size, found[n_found].raw_size);
        ++n_found;
    }
    if (n_found == 0) {
        fprintf(stderr, "no wanted .debug_* sections found in source\n");
        return 1;
    }

    /* ---- Parse target ---- */

    PIMAGE_DOS_HEADER tgt_dos = (PIMAGE_DOS_HEADER)tgt;
    PIMAGE_NT_HEADERS64 tgt_nt =
        (PIMAGE_NT_HEADERS64)(tgt + tgt_dos->e_lfanew);

    if (tgt_nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        fprintf(stderr, "target is not x64\n");
        return 1;
    }

    PIMAGE_SECTION_HEADER tgt_secs = IMAGE_FIRST_SECTION(tgt_nt);
    uint16_t tgt_n = tgt_nt->FileHeader.NumberOfSections;

    uint32_t file_align = tgt_nt->OptionalHeader.FileAlignment;
    uint32_t sect_align = tgt_nt->OptionalHeader.SectionAlignment;
    uint32_t size_of_headers = tgt_nt->OptionalHeader.SizeOfHeaders;

    uint8_t *sect_table_end_p = (uint8_t *)(tgt_secs + tgt_n);
    uint32_t sect_table_end_off = (uint32_t)(sect_table_end_p - tgt);
    uint32_t header_free = size_of_headers - sect_table_end_off;
    uint32_t header_need = (uint32_t)n_found * sizeof(IMAGE_SECTION_HEADER);
    printf("target header area: free=%u, need=%u  (each header = %zu bytes)\n",
           header_free, header_need, sizeof(IMAGE_SECTION_HEADER));
    if (header_need > header_free) {
        fprintf(stderr, "not enough header space - spike doesn't expand\n");
        return 1;
    }

    /* ---- Target's existing symbol + string table ---- */
    uint32_t old_symtab_offset = tgt_nt->FileHeader.PointerToSymbolTable;
    uint32_t old_n_symbols    = tgt_nt->FileHeader.NumberOfSymbols;
    uint32_t old_symtab_size  = old_n_symbols * 18;
    uint32_t old_strtab_size  = 0;
    if (old_symtab_offset != 0) {
        memcpy(&old_strtab_size, tgt + old_symtab_offset + old_symtab_size, 4);
    } else {
        old_strtab_size = 4;    /* just the size field */
    }
    printf("target symtab: off=0x%x n_syms=%u strtab_size=%u\n",
           old_symtab_offset, old_n_symbols, old_strtab_size);

    /* Build new string table by extending the old one with our names. */
    uint32_t new_strtab_size = old_strtab_size;
    for (int i = 0; i < n_found; ++i) {
        found[i].strtab_offset = new_strtab_size;
        new_strtab_size += (uint32_t)strlen(found[i].name) + 1;
    }
    uint8_t *new_strtab = (uint8_t *)calloc(1, new_strtab_size);
    if (!new_strtab) { fprintf(stderr, "OOM\n"); return 1; }
    if (old_symtab_offset != 0) {
        memcpy(new_strtab, tgt + old_symtab_offset + old_symtab_size,
               old_strtab_size);
    } else {
        memcpy(new_strtab, &old_strtab_size, 4);  /* just size field = 4 */
    }
    /* Patch new total size at the start of the new strtab */
    memcpy(new_strtab, &new_strtab_size, 4);
    for (int i = 0; i < n_found; ++i) {
        memcpy(new_strtab + found[i].strtab_offset,
               found[i].name, strlen(found[i].name) + 1);
    }

    /* ---- Compute starting VA / file offset for injected sections ---- */
    uint32_t cur_va = 0, cur_fo = 0;
    for (uint16_t i = 0; i < tgt_n; ++i) {
        uint32_t v_end = tgt_secs[i].VirtualAddress +
                         (tgt_secs[i].Misc.VirtualSize >
                          tgt_secs[i].SizeOfRawData
                              ? tgt_secs[i].Misc.VirtualSize
                              : tgt_secs[i].SizeOfRawData);
        if (v_end > cur_va) cur_va = v_end;
        uint32_t f_end = tgt_secs[i].PointerToRawData +
                         tgt_secs[i].SizeOfRawData;
        if (f_end > cur_fo) cur_fo = f_end;
    }
    /* Also account for existing symbol table location if past sections */
    if (old_symtab_offset != 0) {
        uint32_t f_end = old_symtab_offset + old_symtab_size + old_strtab_size;
        if (f_end > cur_fo) cur_fo = f_end;
    }
    if ((uint32_t)tgt_size > cur_fo) cur_fo = (uint32_t)tgt_size;
    cur_va = round_up(cur_va, sect_align);
    cur_fo = round_up(cur_fo, file_align);

    /* Estimate final file size */
    uint32_t section_data_start = cur_fo;
    uint32_t section_data_end = cur_fo;
    for (int i = 0; i < n_found; ++i) {
        section_data_end += round_up(found[i].virt_size, file_align);
    }
    /* Symbol table (if any) + new string table goes at very end */
    uint32_t final_symtab_offset = section_data_end;
    uint32_t final_size = section_data_end + old_symtab_size + new_strtab_size;

    uint8_t *out = (uint8_t *)calloc(1, final_size);
    if (!out) { fprintf(stderr, "OOM\n"); return 1; }
    memcpy(out, tgt, tgt_size);

    PIMAGE_NT_HEADERS64 out_nt =
        (PIMAGE_NT_HEADERS64)(out + tgt_dos->e_lfanew);
    PIMAGE_SECTION_HEADER out_secs = IMAGE_FIRST_SECTION(out_nt);

    /* Copy old symbol entries to new location (if any) */
    if (old_symtab_offset != 0 && old_symtab_size > 0) {
        memcpy(out + final_symtab_offset, tgt + old_symtab_offset,
               old_symtab_size);
    }
    /* Write new string table after the symbol table */
    memcpy(out + final_symtab_offset + old_symtab_size,
           new_strtab, new_strtab_size);

    /* Write section data + section headers */
    cur_fo = section_data_start;
    for (int i = 0; i < n_found; ++i) {
        PIMAGE_SECTION_HEADER nh = &out_secs[tgt_n + i];
        memset(nh, 0, sizeof(*nh));

        /* Section name via "/N" -> string table */
        char namefield[9];
        snprintf(namefield, sizeof(namefield),
                 "/%u", found[i].strtab_offset);
        memset(nh->Name, 0, 8);
        memcpy(nh->Name, namefield, strlen(namefield));

        nh->Misc.VirtualSize  = found[i].virt_size;
        nh->VirtualAddress    = cur_va;
        nh->SizeOfRawData     = round_up(found[i].virt_size, file_align);
        nh->PointerToRawData  = cur_fo;
        nh->Characteristics   =
            IMAGE_SCN_MEM_READ |
            IMAGE_SCN_CNT_INITIALIZED_DATA |
            IMAGE_SCN_MEM_DISCARDABLE;

        printf("inject %-15s name='%s' VA=%08x FO=%08x size=%u\n",
               found[i].name, namefield, cur_va, cur_fo,
               found[i].virt_size);

        memcpy(out + cur_fo, found[i].data, found[i].virt_size);
        cur_va = round_up(cur_va + found[i].virt_size, sect_align);
        cur_fo = round_up(cur_fo + found[i].virt_size, file_align);
    }

    /* Patch headers */
    out_nt->FileHeader.NumberOfSections      = tgt_n + (uint16_t)n_found;
    out_nt->FileHeader.PointerToSymbolTable  = final_symtab_offset;
    /* NumberOfSymbols unchanged (we copied old ones verbatim) */
    out_nt->OptionalHeader.SizeOfImage       = cur_va;
    out_nt->OptionalHeader.CheckSum          = 0;

    printf("output: NumberOfSections=%u SizeOfImage=0x%x final_file=%u\n",
           out_nt->FileHeader.NumberOfSections,
           out_nt->OptionalHeader.SizeOfImage,
           final_size);
    printf("new symtab at 0x%x, new strtab size=%u\n",
           final_symtab_offset, new_strtab_size);

    if (write_file(out_path, out, final_size) != 0) return 1;
    printf("wrote %s\n", out_path);

    free(src); free(tgt); free(out); free(new_strtab);
    return 0;
}
