#!/usr/bin/env bash
# LCP-10. Actual firmware GamePackReader + boot-only selection on a host PC.
# Output KPG1 files contain test tones ONLY; NEVER install for children.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-"$ROOT/build/lcp10"}"
BUILD="$(mkdir -p "$BUILD" && cd "$BUILD" && pwd)"
INC="$ROOT/firmware/components/kid_app/include"
SRC="$ROOT/firmware/components/kid_app/src"
CXX="${CXX:-g++}"
cd "$ROOT"
python3 -m tools.learning_three_course_fixture \
  --output-dir "$BUILD/three-course-fixture"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC/game_course_loader.cc" \
  "$SRC/game_pack_reader.cc" \
  "$SRC/sound_pack_reader.cc" \
  "$SRC/mini_game.cc" \
  "$SRC/icon_choice.cc" \
  "$SRC/memory_store.cc" \
  "$SRC/scene_model.cc" \
  "$ROOT/tests/cpp/test_game_course_loader.cc" \
  -o "$BUILD/test_game_course_loader"
"$BUILD/test_game_course_loader" \
  "$BUILD/three-course-fixture/fixtures" \
  "$BUILD/three-course-fixture/sdcard/games"
echo "PASS LCP10: boot-only course selector host tests (TEST ONLY) in $BUILD"
