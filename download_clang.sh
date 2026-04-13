#!/bin/bash

#git clone --depth 1 --branch llvmorg-19.1.7 https://github.com/llvm/llvm-project.git

cd llvm-project
git apply ../clang-modifications.patch

# Build
cmake -S llvm -B build -G Ninja \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=../llvm-custom
cmake --build build
cmake --install build

# git apply ../clang-modifications.patch