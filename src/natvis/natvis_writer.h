#pragma once

// NatVis sidecar emitter / embedder. The same XML can be used two ways:
//
//   1. EMBEDDED in the PDB itself via PDBFileBuilder::addInjectedSource
//      (the path used by MSVC's link.exe and lld-link). VS native +
//      cppvsdbg auto-read this stream from the PDB at debug time; no
//      filesystem hunt, no launch.json config needed.
//
//   2. Written to disk as a `<basename>.natvis` sidecar next to the
//      PDB. VS native scans the PDB directory for *.natvis; cppvsdbg
//      in VSCode requires explicit `visualizerFile` in launch.json
//      pointing at the file. Mostly kept for inspection / external
//      tools -- the embedded path is the primary one.
//
// Tier 1 (current): for each Class aggregate, emit a <Type> block
// with a DisplayString naming the class and an Expand that chains
// to the base class (via ExpandedItem) + lists own fields + lists
// 0-argument function methods as Items (which VS evaluates as a
// method invocation when the user expands the class instance).
//
// Tier 2 (future): once we parse RSM property records
// (`0x2c <namelen+1> 0x66 <name>...`), add their accessors as
// additional Items here -- the property name maps to either a
// field expression (field-backed) or a method call (getter-backed).

#include "pdb/pdb_writer.h"

#include <string>

namespace rsm2pdb::natvis {

// Build the NatVis XML content for the aggregates in `inputs`.
// Returns the full document as a string (empty if no class
// aggregates were registered -- caller should treat that as
// "skip embedding"). Pure builder; no I/O.
std::string buildNatVisXml(const rsm2pdb::pdb::PdbInputs& inputs);

// Convenience: build + write to `path`. Returns true on success.
bool writeNatVisToFile(const std::string& path,
                       const rsm2pdb::pdb::PdbInputs& inputs,
                       std::string& error_out);

} // namespace rsm2pdb::natvis
