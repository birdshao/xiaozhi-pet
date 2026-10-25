#!/usr/bin/env bash
# Build a host-only scene preview and tests (not flashed to ESP32).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-"$ROOT/build/p1"}"
mkdir -p "$BUILD"
SRC="$ROOT/firmware/components/kid_app/src/scene_model.cc"
INC="$ROOT/firmware/components/kid_app/include"
CXX="${CXX:-g++}"
COMMON=(-std=c++17 -O2 -Wall -Wextra -Werror -I"$INC")
"$CXX" "${COMMON[@]}" "$SRC" "$ROOT/tests/cpp/test_pet_scene.cc" -o "$BUILD/test_pet_scene"
"$BUILD/test_pet_scene"
"$CXX" "${COMMON[@]}" "$SRC" "$ROOT/tools/pet_scene_preview.cc" -o "$BUILD/pet_scene_preview"
for pose in happy sleepy curious reader; do
    "$BUILD/pet_scene_preview" "$pose" "$BUILD/$pose.pbm"
done
echo "Host-only previews available in $BUILD (P4 PBM 800x480)"
