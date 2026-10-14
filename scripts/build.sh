#!/usr/bin/env bash
# build.sh — configure + build with CMake into build/.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$REPO" -B "$REPO/build" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
cmake --build "$REPO/build" -j"$(nproc)"
echo "built: $(ls "$REPO"/build/fp_* | xargs -n1 basename | tr '\n' ' ')"
