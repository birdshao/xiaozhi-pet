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

# End-to-end pet event -> scene model -> monochrome buffer.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/firmware/components/kid_app/src/scene_model.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_scene_presenter.cc" \
  "$ROOT/tests/cpp/test_pet_scene_presenter.cc" -o "$BUILD/test_pet_scene_presenter"
"$BUILD/test_pet_scene_presenter"

# Byte-stable checkpoint codec, last-128 event journal, and simulated dual-slot NVS.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/tests/cpp/test_pet_persistence.cc" -o "$BUILD/test_pet_persistence"
"$BUILD/test_pet_persistence"

# Critical events must not ACK/play sounds until the NVS transaction commits.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_event_executor.cc" \
  "$ROOT/tests/cpp/test_pet_event_executor.cc" -o "$BUILD/test_pet_event_executor"
"$BUILD/test_pet_event_executor"

# Device-independent serial event pump with bounded queue and storage retry.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_event_executor.cc" \
  "$ROOT/tests/cpp/test_pet_event_runtime.cc" -o "$BUILD/test_pet_event_runtime"
"$BUILD/test_pet_event_runtime"

# Local input IDs share durable checkpoint generation, not volatile boot counter.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/tests/cpp/test_pet_local_event_id.cc" -o "$BUILD/test_pet_local_event_id"
"$BUILD/test_pet_local_event_id"
