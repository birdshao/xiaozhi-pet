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

# Audio policy is platform-neutral; execute with the same P2 host suite.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/audio_coordinator.cc" \
  "$ROOT/tests/cpp/test_audio_coordinator.cc" -o "$BUILD/test_audio_coordinator"
"$BUILD/test_audio_coordinator"

# E-paper full-refresh pacing and scene coalescing without any framebuffer heap.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_refresh_gate.cc" \
  "$ROOT/tests/cpp/test_pet_refresh_gate.cc" -o "$BUILD/test_pet_refresh_gate"
"$BUILD/test_pet_refresh_gate"

# Complete offline pipeline: durable action -> scene projection -> EPD pacing.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_event_executor.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_scene_presenter.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_refresh_gate.cc" \
  "$ROOT/tests/cpp/test_pet_device_flow.cc" -o "$BUILD/test_pet_device_flow"
"$BUILD/test_pet_device_flow"

# P3 offline-first DeviceBridge: persistent outbound events, fixed HTTP
# envelope and authenticated ACK policy (real HTTPS worker not yet attached).
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_device_bridge.cc" \
  "$ROOT/tests/cpp/test_pet_device_bridge.cc" -o "$BUILD/test_pet_device_bridge"
"$BUILD/test_pet_device_bridge"

# Transport-agnostic network sender: bounded retry, strong ACK, no direct HTTP.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_device_bridge.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_device_bridge_sender.cc" \
  "$ROOT/tests/cpp/test_pet_device_bridge_sender.cc" \
  -o "$BUILD/test_pet_device_bridge_sender"
"$BUILD/test_pet_device_bridge_sender"
