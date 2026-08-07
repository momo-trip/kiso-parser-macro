cmake_minimum_required(VERSION 3.13)
project(MacroFinder)

# Use custom-built version
get_filename_component(LLVM_CUSTOM "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-custom" ABSOLUTE)

if(NOT EXISTS "${LLVM_CUSTOM}/lib/cmake/llvm")
  message(FATAL_ERROR "llvm-custom not found at ${LLVM_CUSTOM}")
endif()

set(CMAKE_PREFIX_PATH "${LLVM_CUSTOM}")
set(LLVM_DIR "${LLVM_CUSTOM}/lib/cmake/llvm")
set(Clang_DIR "${LLVM_CUSTOM}/lib/cmake/clang")
message(STATUS "Using LLVM at ${LLVM_DIR}")


find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "LLVM version: ${LLVM_VERSION}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
include_directories(SYSTEM ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Added: Disable RTTI
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

add_executable(stripe_line strip_line.cpp)

llvm_map_components_to_libnames(llvm_libs 
  support 
  core
  mc
  option
)

target_link_libraries(stripe_line
  clangTooling
  clangFrontend
  clangParse
  clangSema
  clangAnalysis
  clangAST
  clangBasic
  clangLex
  clangSerialization
  clangAPINotes
  clangEdit
  clangRewrite
  clangDriver
  clangFormat
  clangToolingCore
  clangToolingInclusions
  clangASTMatchers
  clangSupport
  ${llvm_libs}
)