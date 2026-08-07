#!/bin/bash

rm -rf build
mkdir build && cd build
#CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ..
cmake -DCMAKE_PREFIX_PATH=~/macrust/llvm-custom ..
make
cd ..