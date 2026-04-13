#!/bin/bash

git clone --depth 1 --branch llvmorg-19.1.7 https://github.com/llvm/llvm-project.git

cd llvm-project
git apply ../clang-modifications.patch

# Build
# rm -rf build
cmake -S llvm -B build -G Ninja \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../llvm-custom
cmake --build build --parallel 2
cmake --install build
