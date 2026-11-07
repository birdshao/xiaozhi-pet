#!/usr/bin/env bash
# Build/test firmware's native KPG1 + InputHub + SceneComposer without ESP32.
# Only synthetic tone fixtures. The result must never be deployed as course data.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-"$ROOT/build/lcp09"}"
mkdir -p "$BUILD"
BUILD="$(cd "$BUILD" && pwd)"
cd "$ROOT"
INC="$ROOT/firmware/components/kid_app/include"
SRC="$ROOT/firmware/components/kid_app/src"
CXX="${CXX:-g++}"
python3 -m tools.learning_math_native_fixture --output-dir "$BUILD/device-fixture" \
  --narration "${LCP09_NARRATION:-synthetic}"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC/game_pack_reader.cc" \
  "$SRC/sound_pack_reader.cc" \
  "$SRC/mini_game.cc" \
  "$SRC/icon_choice.cc" \
  "$SRC/memory_store.cc" \
  "$SRC/scene_model.cc" \
  "$SRC/input_hub.cc" \
  "$ROOT/tests/cpp/test_learning_math_device_flow.cc" \
  -o "$BUILD/test_learning_math_device_flow"
"$BUILD/test_learning_math_device_flow" \
  "$BUILD/device-fixture/games/default.kpg" \
  "$BUILD/true-gameart-previews"
python3 -m tools.learning_math_native_report \
  --source "$BUILD/true-gameart-previews" \
  --output "$BUILD/previews" \
  --lesson "$BUILD/device-fixture/source/lesson.json" \
  --narration "${LCP09_NARRATION:-synthetic}"
echo "PASS: host KPG1/device logic integration (TEST ONLY) in $BUILD"
