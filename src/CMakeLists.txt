add_library(rsm2pdb_core STATIC
  rsm/rsm_reader.cpp
  rsm/rsm_primitives.cpp
  rsm/rsm_procs_vars.cpp
  rsm/rsm_unit_tables.cpp
  rsm/rsm_aggregates.cpp
  rsm/rsm_dump.cpp
  rsm/rsm_decorate.cpp
  map/map_reader.cpp
  model/model.cpp
  compose/frame.cpp
  dwarf/dwarf_emitter.cpp
  pe/pe_injector.cpp
  pe/pe_pdb_injector.cpp
  pdb/pdb_writer.cpp
  cli/cli_usage.cpp
  cli/cli_cmd_dump.cpp
  cli/cli_cmd_diff_procs.cpp
  cli/cli_cmd_probe_procs.cpp
  cli/cli_cmd_analyze_procs.cpp
  cli/cli_cmd_dwarf_emit.cpp
  cli/cli_cmd_dwarf.cpp
  cli/cli_cmd_pdb.cpp
)

target_include_directories(rsm2pdb_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rsm2pdb_core PUBLIC ${RSM2PDB_LLVM_LIBS})

add_executable(rsm2pdb main.cpp)
target_link_libraries(rsm2pdb PRIVATE rsm2pdb_core)
