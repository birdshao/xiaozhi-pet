# P3 PetLearning service MVP

Independent Python backend for the chosen architecture B: XiaoZhi ESP32 firmware + self-hosted XiaoZhi voice server + separate PetLearning service. This PR implements the BUSINESS LAYER, not an OTA process.

## Implemented

- SQLite durable per-device event journal with permanent idempotency receipts, monotonic server seq, distinct device-scoped bearer tokens, and approved-story progress projection.
- Deterministic voice intent proposals for feeding, next page and exit story; approved page text or conservative fixed TTS fallback. Arbitrary ASR/LLM strings are never echoed to speech; the LLM cannot grant rewards.
- Manually reviewed page storage, with unpublished drafts hidden from device and voice endpoints.
- Default-disabled generative chat, local quiet mode, no raw audio or transcript storage.

## Try locally (Python 3.10+)

From the repository root:

1. Install dependencies: python -m pip install -r services/requirements.txt
2. Set KID_PET_DEVICE_TOKEN to a unique secret at least 32 characters long (do not type secrets as command-line arguments).
3. Provision: python -m tools.pet_learning_admin enroll --device-id pet-unit-001
4. Import a DRAFT page: python -m tools.pet_learning_admin import-page --book-id animals_001 --title 动物朋友 --page 1 --text 这是一只小象。
5. After a trusted adult manually checks the content, repeat step 4 and add --approved to publish that page.
6. Run on loopback: python -m uvicorn services.pet_learning.api:app --host 127.0.0.1 --port 8765
7. Test: python -m unittest discover -s tests -p test_p3_pet_learning_api.py -v

Do not expose the MVP to the public Internet. A future LAN deployment needs TLS, explicit enrollment/credential rotation, rate limiting and parent authorization. No child should use this prototype without adult supervision.

## API contract

Authorization: Bearer <per-device-token>. Tokens are provisioned offline; there is no network-accessible administrative interface.

POST /v1/devices/pet-unit-001/events accepts a batch of 1–32 events matching this JSON:

    {"events":[{"schema_version":"1.0","message_id":"pet-unit-001:1","device_id":"pet-unit-001","seq":1,"type":"pet.event","payload":{"event":"PET_FEED"}}]}

ACK example:

    {"acks":[{"message_id":"pet-unit-001:1","accepted":true,"server_state_version":1,"reason_code":null}]}

A byte-equivalent logical event resent with the same immutable envelope returns DUPLICATE and the original version, with no new rewards. Reusing a message ID with changed content returns MESSAGE_ID_CONFLICT. Skipped sequence numbers and old events are rejected. The ESP32 PetController remains the owner of instant offline actions; this service maintains a synchronized projection, never replays rewards on the device.

GET /v1/devices/{id}/snapshot returns the device-scoped minimal pet state.
GET /v1/books/{book_id}/pages/{page} needs the bearer token plus X-Device-Id and returns only approved pages. Drafts and missing pages both return 404.
POST /v1/devices/{id}/intent with {"text":"下一页"} returns a PROPOSED deterministic action plus a reviewed TTS phrase. It does not mutate device state. Unknown utterances use fixed fallback; there is no general LLM safety guarantee or streaming TTS here.

Event types: PET_PICKUP, PET_FEED, STORY_OPEN (requires an approved book first page), PAGE_NEXT, STORY_STOP, VOICE_STARTED, VOICE_ENDED, QUIET_HOURS_START and QUIET_HOURS_END. Do not let an LLM fabricate events, device IDs, reward amounts or approvals.

## Important integration limitation

The existing ESP32 firmware does not yet post these HTTP events. The DeviceBridge with bounded offline queue, replay reconciliation and scoped authentication must be developed next. A locally durably committed action rejected by the server must not be silently discarded or renumbered: reconcile policy/version first. Self-hosted XiaoZhi ASR and a fully audited final-TTS gate are also pending. Successful Python tests do NOT prove end-to-end voice or ePaper hardware behavior.
