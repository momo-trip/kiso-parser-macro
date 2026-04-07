#!/bin/bash

rm -rf build
mkdir build && cd build
# cmake ..
cmake -DCMAKE_PREFIX_PATH=~/macrust/llvm-custom ..
make
cd ..


# ./build/macro_analyzer /home/ubuntu/c_parser/test.c -- > output.json
# #./macro_analyzer /path/to/test.c -- > output.json

# ./build/macro_analyzer /home/ubuntu/macrust/programs/mini/main.c -- > output.json