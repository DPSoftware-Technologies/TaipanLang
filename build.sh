#!/bin/sh
# Build the Taipan standalone runner with any C++17 compiler (g++ or clang++).
#   ./build.sh            optimized build -> build/taipan and build/taipan-lsp
#   ./build.sh debug      debug build with symbols
set -e
CXX="${CXX:-g++}"
FLAGS="-std=c++17 -O2"
[ "$1" = "debug" ] && FLAGS="-std=c++17 -O0 -g -Wall -Wextra"
mkdir -p build
$CXX $FLAGS -Iinclude src/*.cpp tools/taipan_main.cpp -o build/taipan
$CXX $FLAGS -Iinclude -Isrc src/*.cpp tools/taipan_lsp.cpp -o build/taipan-lsp
echo "built build/taipan and build/taipan-lsp"
