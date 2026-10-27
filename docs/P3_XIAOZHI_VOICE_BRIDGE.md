# P3: self-hosted XiaoZhi ASR-to-PetLearning bridge (opt-in)

This is a narrow, REVIEWABLE integration into the existing Python XiaoZhi voice server, not a replacement WebSocket/Opus/ASR/TTS server.

Upstream reference: xinnan-tech/xiaozhi-esp32-server, commit f1965167cc2993556cd3e963f8657e2686006c77. The verified speech path is core/handle/receiveAudioHandle.py -> core/handle/intentHandler.py::handle_user_intent(conn,text) -> (if unhandled) ordinary LLM conversation. The existing speak_txt uses the normal XiaoZhi TTS queue. Our adapter inserts a **child-mode pre-LLM gate** in handle_user_intent; all ASR content is handled in child mode, including errors. Normal adult/non-child mode returns False and continues unchanged.

Implemented files:
- integrations/xiaozhi_server/child_voice_guard.py: async read of local PetLearning intent API with a 3-second request timeout, exact hardware-device binding, one per-device bearer credential in environment, no raw transcript logging. Only a reviewed fixed reply or approved page reaches upstream speak_txt. Failed or unavailable API => fixed spoken error, **never fallback to LLM**.
- scripts/p3_server_overlay.py: requires exact pinned upstream Git commit, applies a single ASR guard hook and copies the module to plugins_func/functions/child_voice_guard.py inside an explicitly disposable server checkout. Default is dry-run; --apply modifies only that checkout. Rejects upstream anchor drift and duplicate injections.
- tests/test_p3_xiaozhi_child_voice_guard.py: in-process FastAPI exercise and mocks for device binding, network failures, normal-mode no-op, kid-mode controlled TTS, and syntax-safe one-time overlay.

Develop on a separate local clone of the pinned upstream commit and a separate local PetLearning process:
1. Set KID_PET_CHILD_MODE=1 only in the opted-in development server, else the guard returns False.
2. Set KID_PET_UPSTREAM_DEVICE_ID to the board's actual XiaoZhi device-id header value, and KID_PET_BUSINESS_DEVICE_ID to its separately provisioned PetLearning device ID.
3. Set KID_PET_DEVICE_TOKEN securely (same per-device secret registered in the PetLearning SQLite store). Keep it out of args, GitHub, device flash and logs.
4. Set KID_PET_API_URL=http://127.0.0.1:8765 (only local loopback is accepted in this first MVP).
5. Validate the disposable server checkout: python scripts/p3_server_overlay.py --source /path/to/xiaozhi-esp32-server/main/xiaozhi-server
6. Only in the disposable source checkout, apply: python scripts/p3_server_overlay.py --source /path/to/xiaozhi-esp32-server/main/xiaozhi-server --apply
7. In a controlled parent-supervised test, run the pinned upstream server with local PetLearning API.

This guard intentionally NEVER directly feeds/changes reward state when a child asks for feeding or page navigation; it says to use the physical buttons, because the device-side trusted action/ACK bridge is not yet connected. Non-command text goes to a conservative fixed fallback, not a general AI model. No full child-safe LLM content filtering, model voice conversation or automated story dialogue is claimed.

Limitations: only the exact pinned upstream commit has an assumed ASR hook. This patch has not been deployed to a real XiaoZhi server or tested with the ESP32 microphone/speaker. The hook does not cover every other possible upstream TTS source, such as a server-generated welcome, error or farewell; do not enable unsupervised child use without a full TTS sink audit and the separate device bridge.
