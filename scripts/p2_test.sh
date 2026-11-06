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

# Single atomic NVS blob: pet action+outbound event AND remote ACK dequeue.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$SRC" "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_atomic_ledger.cc" \
  "$ROOT/firmware/components/kid_app/src/parent_settings.cc" \
  "$ROOT/firmware/components/kid_app/src/rhythm_scheduler.cc" \
  "$ROOT/firmware/components/kid_app/src/rtc_clock.cc" \
  "$ROOT/tests/cpp/test_pet_atomic_ledger.cc" \
  -o "$BUILD/test_pet_atomic_ledger"
"$BUILD/test_pet_atomic_ledger"

# Board BOOT gestures: home interaction, story start/advance/exit, no
# accidental feed while reading or interaction in quiet/voice states.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_scene_presenter.cc" \
  "$ROOT/firmware/components/kid_app/src/input_hub.cc" \
  "$ROOT/firmware/components/kid_app/src/home_menu.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_input_router.cc" \
  "$ROOT/tests/cpp/test_pet_input_router.cc" \
  -o "$BUILD/test_pet_input_router"
"$BUILD/test_pet_input_router"

# V2 home action rail: clamped selection and item -> Pet action mapping.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/firmware/components/kid_app/src/input_hub.cc" \
  "$ROOT/firmware/components/kid_app/src/home_menu.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_input_router.cc" \
  "$ROOT/tests/cpp/test_home_menu.cc" \
  -o "$BUILD/test_home_menu"
"$BUILD/test_home_menu"

# ePaper refresh planner: partial for highlights, expressions and poses,
# full for scene/page changes and failures; per-region ghosting budgets.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/refresh_planner.cc" \
  "$ROOT/tests/cpp/test_refresh_planner.cc" \
  -o "$BUILD/test_refresh_planner"
"$BUILD/test_refresh_planner"

# Three-way dial / BOOT / PWR semantics: press-down feedback, bounded
# auto-repeat, long press, parent combo and voice-session gating.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/input_hub.cc" \
  "$ROOT/tests/cpp/test_input_hub.cc" \
  -o "$BUILD/test_input_hub"
"$BUILD/test_input_hub"

# V1 round 3 diagnostics: PCF85063 BCD/alarm codec, civil time math and
# accelerometer window statistics used for gesture thresholds.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/rtc_clock.cc" \
  "$ROOT/firmware/components/kid_app/src/motion_stats.cc" \
  "$ROOT/tests/cpp/test_v1_diag_codecs.cc" \
  -o "$BUILD/test_v1_diag_codecs"
"$BUILD/test_v1_diag_codecs"

# Host-scripted hardware self-test: PETSELFTEST1 parser and rejection rules.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/selftest_script.cc" \
  "$ROOT/tests/cpp/test_selftest_script.cc" \
  -o "$BUILD/test_selftest_script"
"$BUILD/test_selftest_script"

# Test-only self-test time scale: real time by default, 10..100 %, and the
# feature timers that wrap it (blink, cleanup, power, parent exit, battery).
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/selftest_script.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_animation.cc" \
  "$ROOT/firmware/components/kid_app/src/power_manager.cc" \
  "$ROOT/firmware/components/kid_app/src/parent_menu.cc" \
  "$ROOT/firmware/components/kid_app/src/parent_settings.cc" \
  "$ROOT/firmware/components/kid_app/src/battery_policy.cc" \
  "$ROOT/firmware/components/kid_app/src/rhythm_scheduler.cc" \
  "$ROOT/firmware/components/kid_app/src/rtc_clock.cc" \
  "$ROOT/tests/cpp/test_selftest_clock.cc" \
  -o "$BUILD/test_selftest_clock"
"$BUILD/test_selftest_clock"

# V2-09 sleep flow: show, 3 s cancel window after display, power off once.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/sleep_flow.cc" \
  "$ROOT/tests/cpp/test_sleep_flow.cc" \
  -o "$BUILD/test_sleep_flow"
"$BUILD/test_sleep_flow"

# V2-07 option prompts: strict 24 kHz mono PCM16 WAV clips and
# "speak the item the child stops on" scheduling.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/wav_clip.cc" \
  "$ROOT/firmware/components/kid_app/src/prompt_scheduler.cc" \
  "$ROOT/tests/cpp/test_option_prompts.cc" \
  -o "$BUILD/test_option_prompts"
"$BUILD/test_option_prompts"

# V3-01 rhythm: day phases, quiet hours across midnight, next RTC wake-up.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/rtc_clock.cc" \
  "$ROOT/firmware/components/kid_app/src/rhythm_scheduler.cc" \
  "$ROOT/tests/cpp/test_rhythm_scheduler.cc" \
  -o "$BUILD/test_rhythm_scheduler"
"$BUILD/test_rhythm_scheduler"

# V4-01 gestures from accelerometer windows (V1 round 3 thresholds):
# pickup, gentle/hard shake, face down/up, cooldowns, disable.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/gesture_detector.cc" \
  "$ROOT/tests/cpp/test_gesture_detector.cc" \
  -o "$BUILD/test_gesture_detector"
"$BUILD/test_gesture_detector"

# V2-05 status bar: packed status word, readings -> shown values with
# hysteresis, SHTC3 CRC and the once-a-minute status refresh schedule.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/status_bar.cc" \
  "$ROOT/tests/cpp/test_status_bar.cc" \
  -o "$BUILD/test_status_bar"
"$BUILD/test_status_bar"

# V3-03 PowerManager (Active/Idle/Nap/Off, night and battery rules) and the
# V3-04 morning window: back to sleep 60 s after an alarm wake.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/power_manager.cc" \
  "$ROOT/tests/cpp/test_power_manager.cc" \
  -o "$BUILD/test_power_manager"
"$BUILD/test_power_manager"

# Short ePaper animations (incl. the V3-04 good-morning one), idle blink
# and deferred ghosting cleanup.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_animation.cc" \
  "$ROOT/tests/cpp/test_pet_animation.cc" \
  -o "$BUILD/test_pet_animation"
"$BUILD/test_pet_animation"

# V5-02/03 memory: today/likes/growth/recent books, dial-only likes guard,
# ledger v2 with v1 migration, replay idempotence, parent wipe, voice summary.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_atomic_ledger.cc" \
  "$ROOT/firmware/components/kid_app/src/parent_settings.cc" \
  "$ROOT/firmware/components/kid_app/src/rhythm_scheduler.cc" \
  "$ROOT/firmware/components/kid_app/src/rtc_clock.cc" \
  "$ROOT/tests/cpp/test_pet_memory.cc" \
  -o "$BUILD/test_pet_memory"
"$BUILD/test_pet_memory"

# V5-01 personality: fixed temperament parameters, reviewed phrase ids and
# home-item proposals; same input -> same output.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/personality.cc" \
  "$ROOT/tests/cpp/test_personality.cc" \
  -o "$BUILD/test_personality"
"$BUILD/test_personality"

# V6-01 voice faces: XiaoZhi listening / thinking / speaking -> home face,
# settled and rate-limited for the ePaper (no refresh storm).
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/voice_face.cc" \
  "$ROOT/tests/cpp/test_voice_face.cc" \
  -o "$BUILD/test_voice_face"
"$BUILD/test_voice_face"

# V2-08 TF sound pack (KPS1) + built-in fallback, V5-04 pet lines chosen
# through Personality. Cross-checked against a pack made by the Python tool.
PYTHONPATH="$ROOT" python3 -m tools.sound_pack demo --output "$BUILD/sound-demo" >/dev/null
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/sound_pack_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/sound_bank.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_phrases.cc" \
  "$ROOT/firmware/components/kid_app/src/personality.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/wav_clip.cc" \
  "$ROOT/tests/cpp/test_sound_pack.cc" \
  -o "$BUILD/test_sound_pack"
"$BUILD/test_sound_pack" "$BUILD/sound-demo/sounds"

# V4-04 KPB2 content packs: grey pages, dial choice questions, like -> memory
# mapping and KPB1 compatibility, read from the golden packs the tool made.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/content_pack_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/story_pack_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/story_pack_file_port.cc" \
  "$ROOT/firmware/components/kid_app/src/scene_model.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/tests/cpp/test_content_pack_reader.cc" \
  -o "$BUILD/test_content_pack_reader"
"$BUILD/test_content_pack_reader" "$ROOT/tests/fixtures/kpb2"

# V3-05 low battery and charging: debounced plug/unplug, low/critical bands
# with hysteresis, critical retry, rate-limited eat/yawn reactions.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/battery_policy.cc" \
  "$ROOT/tests/cpp/test_battery_policy.cc" \
  -o "$BUILD/test_battery_policy"
"$BUILD/test_battery_policy"

# V4-03 environment life tips: hot/cold bands with hysteresis and a hold,
# board-heat offset, quiet hours, daily limit, reviewed tip phrases.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/env_tips.cc" \
  "$ROOT/firmware/components/kid_app/src/personality.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_phrases.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/tests/cpp/test_env_tips.cc" \
  -o "$BUILD/test_env_tips"
"$BUILD/test_env_tips"

# V3-06 parent menu: settings validation and codec, volume cap, dial-driven
# menu (edit/save/long-press/30 s exit), ledger v3 with v2 migration.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_atomic_ledger.cc" \
  "$ROOT/firmware/components/kid_app/src/parent_settings.cc" \
  "$ROOT/firmware/components/kid_app/src/parent_menu.cc" \
  "$ROOT/firmware/components/kid_app/src/rhythm_scheduler.cc" \
  "$ROOT/firmware/components/kid_app/src/rtc_clock.cc" \
  "$ROOT/tests/cpp/test_parent_settings.cc" \
  -o "$BUILD/test_parent_settings"
"$BUILD/test_parent_settings"

# Board switch to the atomic ledger: one-time verified migration from the
# legacy kid_pet checkpoint and kid_parent/settings (power loss at every
# step, idempotent, legacy keys never written), local-only (no outbox) mode.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_persistence.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_atomic_ledger.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_ledger_migration.cc" \
  "$ROOT/firmware/components/kid_app/src/parent_settings.cc" \
  "$ROOT/firmware/components/kid_app/src/rhythm_scheduler.cc" \
  "$ROOT/firmware/components/kid_app/src/rtc_clock.cc" \
  "$ROOT/tests/cpp/test_pet_ledger_migration.cc" \
  -o "$BUILD/test_pet_ledger_migration"
"$BUILD/test_pet_ledger_migration"

# V4-05 picture-book reader: dial state machine (pages, narration, read
# again, questions, end, home), like-write guard, overlay frames and 4-grey
# planes on the golden packs and on the reader self-test book the tool makes.
PYTHONPATH="$ROOT" python3 -m tools.make_reader_test_book --output "$BUILD/reader-test" >/dev/null
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/book_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/icon_choice.cc" \
  "$ROOT/firmware/components/kid_app/src/content_pack_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/story_pack_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/story_pack_file_port.cc" \
  "$ROOT/firmware/components/kid_app/src/scene_model.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/tests/cpp/test_book_reader.cc" \
  -o "$BUILD/test_book_reader"
"$BUILD/test_book_reader" "$ROOT/tests/fixtures/kpb2" "$BUILD/reader-test/books"

# V4-06 mini games: built-in rounds, deterministic sessions, rewards only
# for correct dial answers, growth memory, tones, game scene and its
# refresh regions; KPG1 game packs read from the golden pack the tool made.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/mini_game.cc" \
  "$ROOT/firmware/components/kid_app/src/icon_choice.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/scene_model.cc" \
  "$ROOT/firmware/components/kid_app/src/refresh_planner.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_refresh_gate.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/tests/cpp/test_mini_game.cc" \
  -o "$BUILD/test_mini_game"
"$BUILD/test_mini_game"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/game_pack_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/sound_pack_reader.cc" \
  "$ROOT/firmware/components/kid_app/src/mini_game.cc" \
  "$ROOT/firmware/components/kid_app/src/icon_choice.cc" \
  "$ROOT/firmware/components/kid_app/src/scene_model.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/tests/cpp/test_game_pack_reader.cc" \
  -o "$BUILD/test_game_pack_reader"
"$BUILD/test_game_pack_reader" "$ROOT/tests/fixtures/kpg1"

# V4-08 microphone games: envelope classifier (blow / clap / speech / noise
# from synthetic RMS sequences, no audio), candle and clap-rhythm sessions,
# growth memory rule, game scene regions.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/mic_envelope.cc" \
  "$ROOT/firmware/components/kid_app/src/mic_game.cc" \
  "$ROOT/firmware/components/kid_app/src/mini_game.cc" \
  "$ROOT/firmware/components/kid_app/src/icon_choice.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/scene_model.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/tests/cpp/test_mic_games.cc" \
  -o "$BUILD/test_mic_games"
"$BUILD/test_mic_games"

# V4-07 motion games: tilt tracker, ball and peekaboo sessions, the game
# menu, rewards only for light deliberate motion, growth memory, and the
# scene regions of the ball track and the peekaboo card.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/motion_game.cc" \
  "$ROOT/firmware/components/kid_app/src/mini_game.cc" \
  "$ROOT/firmware/components/kid_app/src/icon_choice.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/scene_model.cc" \
  "$ROOT/firmware/components/kid_app/src/refresh_planner.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_refresh_gate.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/tests/cpp/test_motion_game.cc" \
  -o "$BUILD/test_motion_game"
"$BUILD/test_motion_game"

# V5-04 memory-driven lines: yesterday's book at good morning, a chosen like
# on pickup; frequency cap, quiet hours, no-memory fallback.
"$CXX" -std=c++17 -O2 -Wall -Wextra -Werror -I"$INC" \
  "$ROOT/firmware/components/kid_app/src/memory_phrases.cc" \
  "$ROOT/firmware/components/kid_app/src/personality.cc" \
  "$ROOT/firmware/components/kid_app/src/memory_store.cc" \
  "$ROOT/firmware/components/kid_app/src/pet_controller.cc" \
  "$ROOT/tests/cpp/test_memory_phrases.cc" \
  -o "$BUILD/test_memory_phrases"
"$BUILD/test_memory_phrases"
