#!/bin/bash

git clone --depth 1 --branch llvmorg-19.1.7 https://github.com/llvm/llvm-project.git

# git apply ../clang-modifications.patch