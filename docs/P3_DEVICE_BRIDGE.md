# P3 DeviceBridge outbox — C++ durable core

This code advances the actual child-pet business architecture, not firmware update tooling.

Files:
- firmware/components/kid_app/include/kid_app/pet_device_bridge.h
- firmware/components/kid_app/src/pet_device_bridge.cc
- firmware/components/kid_app/include/kid_app/nvs_bridge_blob_store.h
- firmware/components/kid_app/src/nvs_bridge_blob_store.cc
- tests/cpp/test_pet_device_bridge.cc
- scripts/p2_test.sh

## Implemented

PetDeviceBridge is a single-owner, fixed-memory outbox for already durably committed Pet events. The queue holds 16 pending notifications and writes an integrity-checked v1 state into alternating 416-byte blobs in the separate kid_bridge NVS namespace (outbox_a, outbox_b). Existing kid_pet checkpoint schema and original XiaoZhi NVS keys stay untouched. Loading chooses the highest valid generation; errors reading a slot fail closed; a corrupt newer slot can fall back to a valid older slot.

- AppendCommitted accepts only an already committed event, allocates a monotonically increasing HTTP sequence, and durably stores it before returning Queued. Failed NVS writes do not advance the in-memory sequence. Duplicates still pending are not enqueued again.
- Supported self-contained events: PET_PICKUP, PET_FEED, VOICE_STARTED, VOICE_ENDED, QUIET_HOURS_START, QUIET_HOURS_END. STORY_OPEN, PAGE_NEXT and STORY_STOP are NOT bridged yet, because the current PetEvent does not carry the approved book_id/page context required by Python. We do not guess it.
- Peek returns the oldest event. EncodePostBody produces a stable bounded v1 JSON envelope compatible with the existing PetLearning FastAPI event endpoint. Device ID is validated ASCII; message_id includes the durable local event ID in hexadecimal.
- Acknowledge accepts only a matching head sequence + event ID and positive server ACK with nonzero server version. A server DUPLICATE receipt counts when accepted. The ACK must be committed to NVS before removing the head. HTTP timeouts, wrong ACK, rejected server receipt and NVS failure leave the entry untouched.
- Queue Full never drops an event or consumes a sequence; the producer must explicitly report backpressure.
- Server holds durable receipts, so replay of an older head after NVS recovery is harmless if the exact immutable envelope is reused.

## Explicit remaining integration gaps

The new C++ NVS store and core are compiled into the ESP-IDF kid_app component but NOT constructed/activated by the Waveshare board. No live HTTP worker is included. The current FreertosPetService calls its callback only AFTER PetPersistence::Save, so a power loss between pet commit and outbox enqueue could lose a server notification.

Do not claim end-to-end crash-atomic sync. Before wiring this into firmware callbacks, the outbound event must be committed atomically together with the pet state, via a versioned checkpoint migration or a proven equivalent transaction journal. This also requires a dedicated network worker with bounded HTTP/TLS timeout, credential protection, exact message_id and seq matching, retry backoff and policy conflict reconciliation. A 16-entry queue can fill during long offline use; do not silently deny a child's local interaction or discard already-committed events.

Python PetLearning is a projection, not an instruction to execute a feed twice on the ESP32. Server seq starts at 1 for newly provisioned devices. An existing remote cursor needs authenticated reconciliation before starting; no implicit reset.

## Test

Run: bash scripts/p2_test.sh

Host tests cover crash/restart, unchanged JSON identity, rejected/out-of-order ACK, storage failure, duplicate receipt, full capacity and corrupt NVS slots. ESP-IDF CI compiles the NVS adapter. These are not physical board or deployed voice-server acceptance tests.
