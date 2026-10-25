#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-"$ROOT/build/p2"}"
mkdir -p "$BUILD"
INC="$ROOT/firmware/components/kid_app/include"
SRC="$ROOT/firmware/components/kid_app/src/pet_controller.cc"
CXX="${CXX:-g++}"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/tests/cpp/test_pet_controller.cc" -o "$BUILD/test_pet_controller"
"$BUILD/test_pet_controller"
