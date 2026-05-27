cmake_minimum_required(VERSION 3.27)

# Match the MSVC runtime LLVM was built with (Release = /MD).
# Set BEFORE project() so it propagates to toolchain probes.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    CACHE STRING "" FORCE)
cmake_policy(SET CMP0091 NEW)

project(rsm2pdb LANGUAGES CXX VERSION 0.1.0)

message(STATUS "CMAKE_MSVC_RUNTIME_LIBRARY = ${CMAKE_MSVC_RUNTIME_LIBRARY}")
message(STATUS "CMAKE_POLICY_DEFAULT_CMP0091 = ${CMAKE_POLICY_DEFAULT_CMP0091}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# LLVM ships LLVMConfigVersion.cmake that only matches on exact
# major.minor. We pin to 18.1.x (the .x bound is patch >= requested).
find_package(LLVM 18.1 CONFIG REQUIRED)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

llvm_map_components_to_libnames(RSM2PDB_LLVM_LIBS
  Support
  Object
  BinaryFormat
  DebugInfoDWARF
  DebugInfoPDB
  DebugInfoMSF
  DebugInfoCodeView
  MC
)

option(RSM2PDB_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(src)

if(RSM2PDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
