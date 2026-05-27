include(FetchContent)
# doctest 2.4.11's CMakeLists declares cmake_minimum_required(VERSION 2.8),
# which modern CMake (>= 3.27) refuses. Override the minimum policy
# version just for the FetchContent subbuild.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

add_executable(rsm2pdb_tests
  unit/test_main.cpp
  unit/test_model.cpp
  unit/test_map_reader.cpp
  unit/test_map_reader_fixtures.cpp
  unit/test_dwarf_emitter.cpp
  unit/test_rsm_reader.cpp
  unit/test_pdb_writer.cpp
  unit/test_pe_pdb_injector.cpp
  unit/test_prologue.cpp
)
target_link_libraries(rsm2pdb_tests PRIVATE rsm2pdb_core doctest::doctest)
target_compile_definitions(rsm2pdb_tests PRIVATE
  RSM2PDB_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

add_test(NAME rsm2pdb_tests COMMAND rsm2pdb_tests)
