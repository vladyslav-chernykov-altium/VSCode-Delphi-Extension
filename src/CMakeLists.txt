add_library(rsm2pdb_core STATIC
  rsm/rsm_reader.cpp
  map/map_reader.cpp
  model/model.cpp
  dwarf/dwarf_emitter.cpp
  pe/pe_injector.cpp
)

target_include_directories(rsm2pdb_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(rsm2pdb_core PUBLIC ${RSM2PDB_LLVM_LIBS})

add_executable(rsm2pdb main.cpp)
target_link_libraries(rsm2pdb PRIVATE rsm2pdb_core)
