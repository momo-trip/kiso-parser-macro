cmake_minimum_required(VERSION 3.13)
project(MacroFinder)

# Use custom-built version
set(CMAKE_PREFIX_PATH "/home/ubuntu/macrust/llvm-custom")
set(LLVM_DIR "/home/ubuntu/macrust/llvm-custom/lib/cmake/llvm")
set(Clang_DIR "/home/ubuntu/macrust/llvm-custom/lib/cmake/clang")

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "LLVM version: ${LLVM_VERSION}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
include_directories(SYSTEM ${CLANG_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Added: Disable RTTI
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

add_executable(macro-finder macro_finder.cpp)

llvm_map_components_to_libnames(llvm_libs 
  support 
  core
  mc
  option
)

target_link_libraries(macro-finder
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