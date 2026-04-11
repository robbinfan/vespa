#!/bin/bash
# Convenience build script. Not part of the Vespa build.
set -eu
cd "$(dirname "$0")"
g++ -O2 -std=c++17 -Wall -Wextra -o sort_bench sort_bench.cpp
echo "built: $(pwd)/sort_bench"
