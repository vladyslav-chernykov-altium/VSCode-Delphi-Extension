# Gotchas we hit (worth knowing, may bite again)

Reference knowledge for debugging specific areas. NOT binding rules
-- those live in `CLAUDE.md`. Append-only catalogue of footguns we
tripped on; consult when investigating a related area. New entries
go at the bottom with the next number.

---

1. **Windows Defender / corporate EDR blocks post-link-modified PEs.**
   `objcopy --strip-debug foo.exe` followed by running `foo.exe` gets
   "Access is denied." Even local user-level Defender exclusions are
   overridden by corporate EDR on this machine. **Fix:** integrate
   DWARF injection into the build pipeline so the .exe is "born" with
   DWARF instead of being patched post-hoc. See
   `examples/*/build.cmd` and `vscode-ext/`'s buildRunner.

2. **gdb's Pascal expression parser treats `.` as field access.**
   So a watch on `two_units.S` fails with `-var-create: unable to
   create variable object`. **Fix in our adapter:** variable symbols
   get the UNQUALIFIED local name (`S`), function symbols stay
   qualified (`Geometry.Add` — important for stack traces). See
   `src/map/map_reader.cpp populate()`.

3. **LLVM's CMP0091 NEW behaviour needs to be activated before
   `project()`.** Setting `CMAKE_MSVC_RUNTIME_LIBRARY` after
   `project()` is too late; it needs `set(... CACHE STRING ""
   FORCE)` BEFORE the `project()` call to propagate to the toolchain
   probe. Verify with `grep MSVC_RUNTIME build/CMakeCache.txt` after
   `cmake --fresh`.

4. **doctest 2.4.11's `cmake_minimum_required` is too old** for the
   bundled VS2026 CMake. Workaround: `set(CMAKE_POLICY_VERSION_MINIMUM
   3.5)` in `test/CMakeLists.txt` before the FetchContent call.

5. **DWARF v5 line program state machine** initialises the `File`
   register to 1, but our file table has only entry 0. Without an
   explicit `DW_LNS_set_file 0` at program start, every emitted row
   references file 1 (out of range) and gdb silently drops them.
   See `src/dwarf/dwarf_emitter.cpp writeLineProgram()`.

6. **VSCode's cppdbg only allows breakpoints in known languages.**
   Pascal isn't in that list. Need `"debug.allowBreakpointsEverywhere":
   true` in `.vscode/settings.json`. Without it, gutter clicks on
   `.pas`/`.dpr` files silently do nothing.

7. **`build` task needs `vcvars64.bat` sourced** when invoked from
   VSCode (since VSCode doesn't run the VS Dev shell). Our
   `.vscode/tasks.json` runs `cmd /c "call vcvars64.bat && cmake
   --build ..."` to handle this. If you see `link.exe` complaining
   about missing libs, this is why.

8. **VS2026 (= "VS18") installs at `C:\Program Files\Microsoft
   Visual Studio\18\Insiders\` — NOT `(x86)`.** Legacy `vswhere.exe`
   at `${ProgramFiles(x86)}\Microsoft Visual Studio\Installer\` doesn't
   see VS18. `scripts/install-deps.ps1` has a fallback `Find-VSInstall`
   that probes the new location.

9. **`spike/inject_dwarf.c` is historical only.** The production
   injector is `src/pe/pe_injector.{h,cpp}` — a C++ port that handles
   `SizeOfHeaders` expansion. Don't add new logic to the spike.

10. **Delphi RSM format magic is "CSH7" (not "FB09"/"FB0A").** JCL's
    `JclTD32.pas` reads the legacy TD32 format which is similar in
    spirit but not directly applicable. The RSM parser was largely
    reverse-engineered from scratch. See `rsm-format.txt`.

11. **`rsm2pdb dwarf` requires Delphi to NOT embed TD32 in the EXE**
    (set `DCC_DebugInfoInExe=false`). Otherwise the EXE has a 1+ MB
    `.debug` section that uses up PE header slots, and the injector
    fails with "not enough header space". `examples/*/two_units.dproj`
    has this set correctly. AdvPCB.dproj does NOT set it explicitly
    (defaults on); the in-place injection still worked because the
    DLL had enough header headroom — fortunate, not guaranteed.

12. **CU `low_pc`/`high_pc` MUST include function symbol addresses,
    not just line-entry addresses.** Delphi's compiler-generated
    unit-init function `<Unit>.<Unit>` (the .dpr's `begin..end`
    block) often has sparse / no line entries. If we derive CU
    range only from line entries, that function falls outside the
    range -> gdb returns "no source" for any address in it ->
    VSCode can't navigate. Fixed in `src/dwarf/dwarf_emitter.cpp
    cuRange()` by absorbing function symbol addresses too. Commit
    d6d6abf.

13. **gdb's MI `-environment-directory` accepts a quoted path arg
    as a single C-string and preserves spaces.** The console-syntax
    `directory <path>` does NOT — it splits on whitespace, so
    `C:/Source code` becomes two paths (`C:/Source` and `code`).
    Always use the MI form when emitting source-search dirs from
    the extension. See `vscode-ext/src/extension.ts`.

14. **VSCode cppdbg `sourceFileMap` does prefix substitution, not
    file-equality.** Bare-filename keys like `"AdvPCB.dpr"` only
    work as exact-match prefixes for paths that gdb reports
    unqualified — which depends on whether gdb resolved the path
    or not. Prefer `-environment-directory` so gdb returns
    fullname; sourceFileMap is a useful belt-and-braces but not
    a primary mechanism.

15. **VSCode `DiagnosticSeverity.Hint` does NOT show as a PROBLEMS
    row** — only as inline editor decorations. Delphi compiler
    "Hint" diagnostics must be mapped to `Information` instead to
    surface in the panel. See `vscode-ext/src/buildRunner.ts`.

16. **The PE injector renames pre-existing `.debug_*` sections to
    `.old_dw` before appending fresh ones.** Without this, a DLL
    that's been injected once before ends up with TWO `.debug_info`
    sections; gdb reads the FIRST (stale) one and reports "no
    locals or arguments" even when the new DWARF carries them.
    This bit us on AdvPCB the first time round (the input DLL was
    already an output of a prior `rsm2pdb dwarf` run). See
    `src/pe/pe_injector.cpp` (search for `.old_dw`). Side note:
    each re-injection cycle leaves the old `.old_dw` in place;
    accumulation is harmless but cosmetically ugly.

17. **Real Delphi proc records are MUCH more varied than the small
    fixtures showed.** The original M2-phase-A parser hard-coded
    a fixed 16-byte head (assumed 5-byte trailer between VA and
    sub-records), a single proc subtag value (`0xA0`), and two
    sub-record subtag values (`0x66`/`0x62`). On real binaries
    that's 2% coverage. Reality:
      - Proc subtag ∈ {0xA0, 0xE0, 0x80}.
      - Trailer between VA and first sub-record is variable
        (3..30 bytes; the "5 bytes" was just one common case).
      - Sub-record subtag is variable (~10 values); the strong
        invariant is the two zero bytes at +1,+2 after it.
      - Real procs carry additional sub-records with tags `0x23`
        (return-value descriptor) and `0x25` (enum entries) after
        the param/local list.
    Application rules in `src/rsm/rsm_reader.cpp scanProcedures()`:
    permissive subtag acceptance, scan-forward to find first valid
    sub-record / true end marker, graceful break on unknown sub-
    record tags. See `docs/02-rsm-format-notes.md` (Procedure
    record section) and `rsm-format.txt` (2026-05-26 entry).

18. **`Self` in class methods carries a sentinel `stack_offset`
    (not a real frame offset).** RSM tags Self with a non-primitive
    marker:
      - 0x29 for base-class virtual / regular methods
      - 0x31 for override methods
      - 0xD5 for anonymous-method closure Self (ActRec.$0$Body)
    paired with sentinel offsets `+4109` (newer Delphi) or `+4`
    (older). Don't decode these via the general `sub_rsp + RSM/2`
    formula -- it lands well outside any real frame slot. Instead,
    Self always lives at `rbp + sub_rsp + 16 + 8*extra_pushes` (=
    the rcx-shadow slot, where Delphi spills the implicit `this`-
    pointer; the extra_pushes term shifts it up if the prologue
    pushed callee-saved regs between `push rbp` and `sub rsp`).
    main.cpp routes Self through this special case by checking
    `name == "Self" || marker == 0x29 || marker == 0xD5` -- the
    name fallback also catches 0x31 (override) so we don't need to
    enumerate every marker variant. Verified on examples/04_locals
    against TBase + TDerived virtuals/overrides.

19. **PDB writer (LLVM-backed) gotchas — `cppvsdbg` is strict.**
    Building a PDB that VS native debug actually accepts took
    multiple rounds of fixing. Surface area in
    `src/pdb/pdb_writer.cpp`:
      a. `PDBFileBuilder::initialize()` does NOT allocate the 5
         reserved stream slots. Call
         `getMsfBuilder().addStream(0)` `kSpecialStreamCount` times
         immediately after `initialize()`, or `commit()` fails with
         "stream too short".
      b. DBI requires `DbgStream[SectionHdr]` (the COFF section
         headers verbatim). Without it, vsdbg loads symbols but
         throws "Unexpected symbol reader error" when resolving a
         breakpoint.
      c. File checksums must be a real hash (MD5 minimum); kind
         `None` silently breaks BP resolution in cppvsdbg.
      d. `S_GDATA32` / `S_REGREL32` with `TypeIndex::None()` are
         invisible in Watch/Locals. Until M3-followup ships real
         types, default everything to `Int32` (raw 4-byte view).
      e. `ProcSym.End` must be patched to point at the matching
         `S_END`'s module-stream offset. Use `getNextSymbolOffset()`
         before+after to compute it, then patch the proc record's
         bytes in place (offset 8 = 4-byte CVHeader + 4-byte Parent).
         End=0 means the scope is "open" and Locals stays empty.
      f. SectionContribs must be sorted by `(ISect, Off)` before
         submission — vsdbg binary-searches them. Adding in module
         iteration order leaves SCs interleaved and BP resolution
         fails for whichever module lands at "the wrong" position.
      g. Per-module `mod_index` increment must be UNCONDITIONAL.
         A `continue` that skips it (e.g. for modules without C13
         lines) silently mis-attributes every later module's SCs.
      h. `S_FRAMEPROC` is required for cppvsdbg to resolve
         RegRel-based locals. Encoded frame ptr reg bits 14-15
         (local) and 16-17 (param) must both be 2 (`FramePtr`,
         which decodes to RBP on x64).
      i. cppvsdbg's expression evaluator treats `.` as field
         access. Qualified Pascal names like `two_units.S` parse
         as "field S of two_units" and report "two_units is
         undefined" in Watch. Strip the module prefix in
         `S_GDATA32` names; keep the fully-qualified name in
         `S_PUB32` so stack traces stay informative.
      j. RSDS file checksum / Image file checksum mismatch is
         tolerated by vsdbg as a "source-file changed" warning
         but BPs still bind, so MD5 of the resolved source path
         is good enough.

20. **PDB Section header file offsets don't match the patched
    PE.** The PDB stores the PE's section table BEFORE our RSDS
    section gets appended. After our `injectPdbReference`
    extends the PE (and possibly bumps file offsets via header
    expansion), the file offsets in the PDB's SectionHdr stream
    are stale. They don't matter for runtime resolution (which
    is RVA-based) but `llvm-readobj --coff-debug-directory`
    against the patched PE will sometimes complain about
    "uneven size" if a prior debug directory entry survived.
    Cosmetic; debug works.

21. **PDB source path stored in `C13 FileChecksums`** is what
    cppvsdbg uses to match breakpoint file paths. Delphi's
    `.map` carries only basenames (`Geometry.pas`); we resolve
    against (`--src-search` dirs from the extension) +
    (mapDir/.., mapDir, mapDir/../..) and call
    `std::filesystem::canonical()` so the PDB ends up with
    absolute paths. Without absolute paths, cppvsdbg fails to
    bind BPs even with `sourceFileMap` set — its matcher is
    less flexible than gdb's.

22. **Delphi-x64 prologue variants — `parsePrologue` handles
    both.** The single-block shape `push rbp; sub rsp, imm;
    mov rbp, rsp` is the common case (every proc in
    `examples/04_locals`), but procs with managed locals or many
    spills push callee-saved registers between `push rbp` and
    `sub rsp`:

        push rbp
        push rdi          \  zero..many of these (typical pairs are
        push rsi           > rdi+rsi, or r12..r15 via REX.B + 0x54..)
        sub  rsp, 0x1C0   /
        mov  rbp, rsp

    `parsePrologue` returns `{sub_rsp, extra_pushes}`. Locals don't
    shift (they live at `rbp+0 .. rbp+sub_rsp` regardless of
    pushes), but params + Self shift up by 8 bytes per extra push:
    `real_param = sub_rsp + 8*extra_pushes + RSM/2`. The pushes
    occupy the slots immediately above the local-area, so the
    rcx-shadow slot moves from `rbp + sub_rsp + 16` to `rbp +
    sub_rsp + 16 + 8*extra_pushes`. Verified on
    `examples/05_types ProbeLocals` (2 extra pushes -> all 21
    primitive/string locals resolve correctly in cdb).

23. **Per-unit type table layout (RSM unit metadata block).** Each
    Pascal unit's metadata block in RSM starts with the unit anchor
    `02 <namelen> <unit-name>` and is followed within ~200 bytes by
    a source-file ref `02 70 <namelen> <file>.{pas,dpr,inc,tmp}`.
    The primary type table is a sequence of `0x66` records (each
    `0x66 <namelen> <pascal_name> <4-byte hash>`) in source-
    declaration order, **interleaved with `0x67` records** that
    reference imported functions / methods of the same `<tag>
    <namelen> <name> <hash>` shape -- the scanner walks past them
    without recording. Variable's `type_marker` (always even,
    `0x02 / 0x04 / 0x06 / ...`) indexes the table: marker = `2 *
    (index + 1)`. Looking name up in the hard-coded
    `kPrimitiveTable` (in `rsm_reader.cpp`) gives `(PrimitiveKind,
    byte_size)` -- the pair the pdb_writer needs for typed
    S_REGREL32 / S_GDATA32 emission. False positives are avoided
    by tying every collected table to its unit anchor (per-anchor
    scan with a hard upper bound) rather than free-floating
    "longest run of 0x66" detection, which used to mis-attribute
    variables across units (every marker 0x02 in `examples/
    04_locals` resolved as Boolean before this fix because the
    "locals" unit's anchor-less table was discarded).

24. **Stack-offset 2-byte encoding is SIGNED i16.** Sub-record
    payloads carry the stack offset in either a 1-byte form (LSB=0,
    decoded as `(int8) b0`) or a 2-byte form (LSB=1, two bytes
    `b0, b1` forming a little-endian word). The 2-byte form must be
    interpreted as a **signed** 16-bit value, then `stored_off =
    ((int16) v - 1) / 2` to keep the downstream `sub_rsp + stored
    / 2` formula working. Empirically:

        v=0xFE61 (i16=-415)  -> stored=-208  -> real = sub_rsp + (-104)
        v=0xF9A1 (i16=-1631) -> stored=-816  -> real = sub_rsp + (-408)

    The earlier unsigned read gave `+32560 / +31952` which sent
    these locals well past the end of any real frame. This bites
    string / bool / char / pointer locals in any non-trivial proc
    (their negative-from-top-of-frame offset doesn't fit a signed
    i8). After the fix all 21 of `examples/05_types ProbeLocals`'s
    primitives + strings resolve in cdb -- including
    `lO=true / lCh='Z' / lS="unicode" / lAS="ansi" / ...`.

25. **CodeView string display uses pointer-to-char *simple* types,
    no LF_POINTER record needed.** Combining
    `SimpleTypeKind::WideCharacter / NarrowCharacter` with
    `SimpleTypeMode::NearPointer64` synthesises
    `TypeIndex = T_64PWCHAR / T_64PRCHAR`. cdb and the VS native
    debugger auto-display the pointee as a string starting at the
    address until the first NUL. Pascal RTL conveniently puts char
    data at the pointer's exact address (header bytes live just
    below), so `UnicodeString` / `AnsiString` / `WideString` /
    `UTF8String` / `PChar` / `PAnsiChar` / `PWideChar` all work
    without any extra TPI work. `ShortString` is **not** a managed
    pointer -- it's an inline length-prefixed byte array -- and
    falls back to the size-based `void*` / `byte[N]` chain.
    `WordBool` / `LongBool` use `SimpleTypeKind::Boolean16 /
    Boolean32` which cdb refuses to render (reports "Type
    information missing"); the bit values are still readable via
    `dt` / `dd`. Cosmetic only.

26. **Nested-function subtag is 0x41, not the usual `{A0,E0,80,
    20}`** -- with 3 mystery bytes between subtag and VA (`02 10
    00 <VA u32>`) rather than the usual `00 00 <hash u32> <VA
    u32>`. Parser must accept it without the `00 00`-after-subtag
    invariant. The static link (parent's rbp, passed in rcx) is
    NOT carried as a sub-record; the parser sets
    `ProcedureRecord.has_static_link = true` on subtag 0x41 so
    main.cpp can synthesise a `__frame_outer__` placeholder local
    at `rbp + sub_rsp + 16`. Lambda body procs
    (`<Outer>$ActRec.$0$Body`) use the regular subtag but carry
    Self with marker 0xD5 -- their captured fields live in
    `[Self+0x10..]` and need TPI struct synth for auto-deref
    (Step 12).

27. **Class-header layout is VARIABLE-LENGTH between fixtures.**
    The RSM "vtable size info" block between `47 00 10 00 00
    <own_hash u16>` and the base-class slot grows by one or more
    bytes when a class declares additional methods -- in
    particular Pascal `property` declarations that synthesize
    getter / setter procedures (08_inherit_props's TDog has 4
    methods including GetBarkCount / SetBarkCount versus 07_records
    TCircle's 2 methods, and the intermediate block is 10 vs 9
    bytes).

    The original Phase B.1 detector hard-coded base_hash at
    `i + 18` from the `47` marker, which silently dropped the
    inheritance link on any class above 9 bytes of intermediate
    metadata. Real-world AdvPCB-scale classes hit this constantly.

    Robust detector (since commit 394e12a, src/rsm/rsm_reader.cpp):
    scan forward up to 80 bytes for the `25 3f 00` field-list
    anchor, then read base_hash at `fl_at - 10`. The 8-byte tail
    immediately before the anchor (`00 NN .. fe 00 NN`) is the
    only invariant -- everything between the header and the
    base slot flexes with method count.

28. **`registerAggr` recursion must break cycles to avoid stack
    overflow at AdvPCB scale.** Phase E's recursive aggregate
    registrar (cli_cmd_pdb.cpp) walks the base-class chain +
    composite-typed fields depth-first. On AdvPCB (21k classes,
    deep VCL hierarchies, generic container owner-loops) the
    chain can cycle: A -> base B -> base A or A.f -> B.f -> A.
    Without a cycle guard the recursion blows the 1 MB Windows
    stack (STATUS_STACK_OVERFLOW = 0xC00000FD = exit 3221225725
    observed at registerAggr depth ~thousands).

    Fix: an `aggr_in_progress` set keyed by (unit_anchor_offset,
    own_hash). Inserted at the start of registerAggr, erased
    after `inputs.aggregates.push_back`. Re-entry while the same
    key is in-progress returns nullopt (treated as "no base" /
    "unknown field type" downstream). We DELIBERATELY don't
    reserve an aggregate slot upfront -- the writer's dependency-
    order invariant (children pushed before referrers, so they
    get a smaller TypeIndex) would break if we did. Cycle-
    detection + nullopt is the cheapest path that preserves both
    invariants.

29. **Two RSM sub-record bugs hide locals on real Delphi units.**
    User reported on AdvPCB's `PCBCommands_PCB.TPCBCommands.FileSave`
    that the 8 declared locals (FileName, Format, SaveFormat,
    IsRecovery, DocFormat, ShelvedPolyCount, SilentMode, State)
    were missing from the PDB -- only the 3 params showed up. Two
    issues fixed together (commit XXXXXX):

    (a) `scanProcedures` broke its sub-record loop on the FIRST
        unknown tag (e.g. `0x23` return-value descriptor or `0x25`
        enum entries emitted between params and locals for any
        function that returns / accepts an enum). FileSave's
        return type `TSaveFormat` produces 12 `0x25` enum-entry
        records totalling ~150 bytes that sat between AKind and
        FileName -- the existing 6-byte re-anchor window couldn't
        scan past them. Fix: widen the re-anchor to 1024 bytes
        when the current tag isn't one of 0x20 / 0x21 / 0x22 (or
        the proc-end marker 0x63), and validate candidate
        sub-records via parseSub before accepting.

    (b) `parseSub`'s primitive-vs-composite discriminator was
        `mb0 < 0x40 && (mb0 & 1) == 0` -- too restrictive. AdvPCB's
        per-unit primary type tables run to 100+ entries each, so
        primitive markers go up to ~0x9a (PCBCommands_PCB's
        TDynamicString = marker 0x9a, well above 0x40). FileName
        and DocFormat (both TDynamicString) were mis-classified as
        composite-typed, consuming the wrong byte as the offset
        and corrupting subsequent sub-record alignment. Fix:
        switch to BODY-LENGTH disambiguation. Scan forward from
        the sub-record body for the next plausible record tag
        (0x20 / 0x21 / 0x22 / 0x23 / 0x25 / 0x28 / 0x2a / 0x63);
        body length 5 -> primitive 1B, 7 -> composite 2B, 6 ->
        disambiguate by marker-byte LSB (odd = composite). Works
        for every fixture (07_records, 08_inherit_props) AND for
        AdvPCB's FileSave (all 8 locals + correct TSaveFormat*
        type for the in-proc-emitted enum).

30. **Self ALWAYS needs its type synthesised from the proc name,
    not from RSM's Self encoding.** Pascal's implicit `Self`
    parameter in a class method should display as a typed pointer
    to the enclosing class. RSM tags Self with sentinel markers
    (0x29 / 0xD5 / 0x31) or with a per-unit-encoded 2-byte value
    that doesn't resolve to any aggregate hash -- if we trust
    that decode, Self ends up as `unsigned __int64` and the
    debugger renders it as a raw hex number (user saw
    `Self: 8364525696` on AdvPCB's TPCBCommands.FileSave).

    Fix (cli_cmd_pdb.cpp): for every ResolvedVar with is_self,
    derive the enclosing class name from the proc name's
    second-to-last dot-segment (`Unit.TClass.Method` -> `TClass`)
    and look it up:
      1. same unit, kind=Class
      2. any  unit, kind=Class  (the class can live in an
         interface-only unit)
      3. any  unit, any kind    (AdvPCB-style opaque / forward-
         declared classes -- TPCBCommands has a 0x2a record but
         no class header / fields)
      4. fallback: emit Self as VoidPointer64 (better than
         unsigned __int64 because the debugger knows to follow
         the pointer).

    For pass 3 hits the writer synthesises an EMPTY LF_CLASS
    named after the class -- cdb shows `Self: 0x... [TClass]`
    even though no field info is available. self_opaque_cache
    is keyed by class name so one synthetic LF_CLASS per class
    is emitted, not one per method.

    Performance note: a naive linear scan of rsm_reader.
    aggregates() per Self lookup pushed AdvPCB from 46s to 152s.
    Use a pre-built unordered_multimap<name, AggregateType*> +
    unordered_multimap<(unit_anchor, name), ...> so each Self
    lookup is O(1). Back to baseline 46s.

31. **AdvPCB-style classes carry the NAME and the FIELDS in two
    separate 0x2a records linked by `kind & 0x80` + a secondary
    hash at +7..+8.** Small-fixture classes (07_records, 08) emit
    one 0x2a record per class (kind 0x20) that holds own_hash +
    name + base + fields all together. AdvPCB's classes split:
      - 0x2a NAME record (kind & 0x80, e.g. 0xa8) holds the
        Pascal name and own_hash A; carries a SECONDARY hash B
        at the byte pair +7..+8 from kind.
      - 0x47 CLASS HEADER lives separately with own_hash = B.
      - 0x2c FIELDS are parented to B, not A.

    The two-pass parser must:
      1. Read kind. If `kind & 0x80`, also read linked_hash from
         +7..+8 onto AggregateType::linked_hash.
      2. Index the aggregate under BOTH own_hash AND linked_hash
         in aggr_by_hash_ / aggr_by_unit_hash_ (last-wins, with
         own_hash always preferred). Variables referencing
         either hash resolve to the same aggregate.
      3. Attach pending_fields BY (unit_anchor, parent_hash) --
         the global hash-only attach trips on cross-unit
         collisions (System.TTextRec.own_hash 0x0405 collides
         with PCBCommands_PCB.TPCBCommands.linked_hash 0x0405).
      4. Classifier accepts kind=Class when EITHER own_hash OR
         linked_hash is in class_hashes (the 0x47-header set).

    Verified 81.8% match rate across 47k `kind & 0x80` 0x2a
    records in AdvPCB.rsm (38,666 / 47,261 had linked_hash that
    pointed to an existing 0x47 class header). The remaining
    18% are primitives (Boolean / AnsiChar / Char with kind=0xa8
    but no class header) -- harmless because they don't match
    any class_hash so they stay classified as something else.

    Inherent limitation: RSM emits class fields PER-UNIT-USAGE,
    not the full class layout. TPCBCommands has ~hundreds of
    fields in its full declaration, but PCBCommands_PCB.pas
    only references FDensityPlanes + FECOImplementation, so
    those are the only two fields visible when stepping into
    TPCBCommands.FileSave. To see more fields the user'd need
    to also load PDBs for units that reference the other
    fields (or load the interface-unit declaration's PDB).

32. **Anonymous in-proc `set of` locals need synthesis from a
    nearby enum.** When a Pascal function declares both a NESTED
    `Type X = (...)` enum and a `var r: set of X` local, RSM
    emits the enum as a regular 0x2a record but does NOT emit a
    standalone 0x2a record for the anonymous set type. The
    local's RSM type_hash points at a hash that isn't anywhere
    else in the metadata, so the aggregate registrar returns
    nullopt and the local falls through to void*. User report:
    `r` in `TCircle.Create` (records.dpr after the enum-inline
    edit) showed as `type = 0x0603 (void*)`.

    Fix (cli_cmd_pdb.cpp): when a local has aggregate_hash != 0
    but doesn't resolve, scan the same unit's aggregates for an
    Enum kind whose own_hash is within +1..+16 of the local's
    hash. Empirically Delphi assigns the anonymous set's hash =
    enum_hash + 4 (07_records: TSaveFormat 0x1aa9, r 0x1aad).
    If a match is found, synthesise an LF_BITFIELD struct named
    `set of <EnumName>` using the enum's enumerators (same
    machinery as the named-set path from Phase F+, gotcha #29's
    sibling). Set width = ceil(max_ord / 8) bytes.
    inproc_set_cache keyed by (unit_anchor, enum_own_hash) so
    multiple set-of-same-enum locals share one TPI record.

    Verification: 07_records `r := [eASCII, eBinaryCS]` now
    emits `type = 0x1025 (set of TSaveFormat)`. cdb shows each
    enumerator's bit individually. AdvPCB still 45s.

33. **Unit anchor source-file marker has TWO byte patterns.**
    The anchor scanner originally only accepted `02 70 <fnl>
    <unitname>.pas` as the source-file signature within 200
    bytes of an `02 <nl> <unit>` anchor. Empirically:
       0x02 0x70 ... is used by .dpr-style self-anchors
                    (cross_unit.dpr finds its own .dpr through
                    this).
       0x3c 0x70 ... is used by .pas unit anchors
                    (Shapes / Items / Layout in
                    examples/09_cross_unit; AdvPCB RTL units
                    commonly).
    Without 0x3c acceptance multi-unit projects only register
    the .dpr anchor (Shapes/Items/Layout invisible -> their
    types fall into the global last-wins bucket and collide
    with System types of the same per-unit hash). Fix: accept
    either prefix byte, both followed by 0x70 + valid .pas/.dpr
    file (src/rsm/rsm_reader.cpp, gotcha first observed in
    examples/09_cross_unit).

34. **A unit's 0x66 primary type table has multi-uses
    SEPARATORS.** Between blocks of 0x66/0x67 entries that
    belong to different imported units the table inserts a
    short record:
        63 64 <fnl> <imported_unit_name> 00 00 00
    e.g. examples/09_cross_unit Items.pas carries
    `63 64 07 SysInit 00 00 00` and `63 64 06 Shapes 00 00 00`
    before the 0x66 entries for TPoint / TSize / TColor (the
    Shapes-imported types). Without walking past the separator
    the parser stops at the first non-(0x66/0x67) byte and the
    cross-unit imports get lost. Walk-past fix: when seeing
    `63 64 <fnl> <printable> 00 00 00`, advance by 6 + fnl
    bytes and continue collecting 0x66 entries.

35. **The 0x66 table is the cross-unit IMPORT table; each
    entry carries a globally-unique 4-byte type hash.** Each
    0x66 record's `<4-byte hash trailer>` is a globally-stable
    identifier per Pascal type. Two units that both reference
    Shapes.TPoint carry the same hash4 0x0c6d2cf6 in their
    respective 0x66 tables. The LOW 16 bits of hash4 equal
    the type's own_hash in the DECLARING unit -- that's the
    cross-unit link.

    Cross-unit field resolution (cli_cmd_pdb.cpp X.2):
      1. Field's primitive_marker -> unit's 0x66 entry at
         position marker/2 - 1 -> (name, hash4).
      2. If `name` resolves via resolvePrimitive() -> real
         primitive (Integer / string / ...). Use existing path.
      3. Else: search agg_by_name[name] for an aggregate whose
         own_hash equals hash4 & 0xFFFF. That's the foreign-
         unit declaring aggregate. Register it; nested_aggregate
         points at its TypeIndex.
    With this, `Items.TItem.fPos: TPoint` in 09_cross_unit
    renders as `LF_MEMBER Type=0x1001` where 0x1001 is the
    Shapes.TPoint LF_STRUCTURE -- cdb expands fPos into X / Y
    rather than showing a bare `unsigned`.

36. **A Pascal class declared in one unit shows up under
    FOUR hashes in that unit's metadata.** Surfaced by
    09_cross_unit's TItem and TLayout. The roles:
        NAME       = 0x2a own_hash
        LINKED     = 0x2a's secondary hash at byte +7..+8
                     from kind (only when kind & 0x80)
        CLS_HEADER = 0x47 own_hash
        FLD_PARENT = 0x2c parent_hash
    Empirical formulas (verified on records.dpr, 09_cross_unit,
    AdvPCB):
        FLD_PARENT = ((LINKED & 0xff) << 8) | 0x08
                     (UNIVERSAL across records + classes)
        For CLASSES:
            either LINKED == CLS_HEADER own_hash
                   (AdvPCB direct-match, kind=0xa8)
                or LINKED.low == CLS_HEADER.low
                   (09_cross_unit, kind=0xa8 too but split
                   high bytes)
    Phase X.3 fix: ALIAS the aggregate in aggr_by_unit_hash_
    under all derived hashes (CLS_HEADER own_hash AND
    FLD_PARENT), and FORCE-CLASSIFY as Class when a chain to
    a 0x47 header succeeds. Without the aliasing, 0x2c fields
    parented to FLD_PARENT 0x1208 never find their TItem
    aggregate (which has own_hash 0x424c, NAME hash).

37. **Pascal `property` declarations don't surface directly in
    CodeView -- but they ARE in RSM as 0x31 records, and our
    NatVis sidecar exposes them.**

    CodeView has no `LF_PROPERTY` counterpart. The Delphi compiler
    emits these PDB-visible artifacts for a `property X read fX
    write SetX`:
      - backing FIELD (LF_MEMBER `fX`);
      - accessor METHODS (`GetX` / `SetX` if any -> S_GPROC32 +
        S_PUB32).
    What CodeView WON'T render in Watch is the property name
    itself -- `?? lDog.BarkCount` returns `no member`.

    rsm2pdb closes the gap with the NatVis sidecar (embedded in
    the PDB as an injected source -- see src/natvis/). The RSM
    file DOES carry property records (tag 0x31, see rsm-format.txt
    entry 2026-05-29 / Tier 2). Tier 2.0 parses property NAMES
    from those records and emits NatVis `<Item>` entries with
    naming-heuristic expressions:
      - field `fPropName` exists  -> `<Item Name="PropName">fPropName</Item>`
      - method `GetPropName` exists -> `<Item Name="PropName">GetPropName()</Item>`
    Covers ~95% of real-world Delphi convention; non-conventional
    accessors (custom getter names not matching `Get<PropName>`)
    fall back to the raw field access. Tier 2.1 will do full
    accessor-marker resolution if the heuristic proves insufficient
    on real codebases.

    Verified on examples/09_cross_unit (TItem.Name / .Tag / .Pos /
    .Description, TLayout.Origin / .LeadName / .ItemCount). cdb
    still doesn't see properties (cdb ignores NatVis -- that's a
    cdb-side limit, not ours); VS native + cppvsdbg do.

