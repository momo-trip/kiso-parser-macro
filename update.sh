#!/bin/bash

# download_clang.sh

cd parsers/macro_analyzer && ./build.sh
cd ../..

cd parsers/macro_finder && ./build.sh
cd ../..

