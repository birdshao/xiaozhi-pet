# P3: atomic Pet action and outbound event checkpoint

## Problem closed by this module

In the previous P3 design the ESP32 committed a Pet action to kid_pet NVS and later queued the event separately to kid_bridge. A power loss between those two independent NVS writes could make feeding visible to the child but permanently absent from the server.

The new PetAtomicLedger, its codec, and NvsAtomicPetStore persist *both* in one 1,480-byte A/B NVS blob. An accepted action and its outbound record either both commit or neither commits. A remote ACK removes the outbox entry in the very same kind of durable aggregate write. No send occurs here; the Pet controller remains the owner of local effects.

- Namespace kid_pet_sync, keys joint_a / joint_b; neither old kid_pet nor kid_bridge is overwritten.
- Schema v2 (V5) appends a 288-byte MemoryStore before the CRC (1,768 bytes). v1 1,480-byte blobs still load with empty memory; the next commit writes v2 to the other slot. See V5_PERSONALITY_MEMORY.md.
- Schema v3 (V3-06) appends the 28-byte parent settings block (ParentSettingsCodec: day plan, volume cap, quiet-hours ceiling, motion switch; own tag, version and CRC) before the CRC (1,796 bytes). v1/v2 blobs load with the default settings and the next commit writes v3 to the other slot. `SetParentSettings(id, settings)` shares the Pet replay window, rejects invalid settings and is not a pet interaction. The board keeps its parent settings here (the earlier board-only key `kid_parent/settings` is only read once by the migration below).
- Composite blob embeds the byte-exact verified PetCheckpointCodec, a 16-event FIFO, next server sequence, generation, CRC32 and schema magic. CRC detects corruption, not malicious alteration. Newest valid A/B slot is selected.
- Refuses unknown/corrupt-only NVS and failures reading either slot; never auto-erases.
- Applies feed/pickup/voice-start/voice-end/quiet-start/quiet-end in a single persistent blob. Story events remain local until book_id and approved-page context can be carried in the device event schema.
- If the 16-slot queue is full, projectable child actions are NOT committed or audibly acknowledged, preserving synchronization guarantees. Non-projectable story actions can still work locally. This capacity/buffering tradeoff requires a product policy decision before device rollout.
- Exact server message_id, seq and positive version must be validated before durable ACK dequeue. Lost ACK can safely replay the *same* original event.
- On restart a transitional Listening/Speaking mode is normalized to Idle without corrupting the saved outbox.

## Operational status and migration

**The Waveshare board now runs FreertosAtomicPetService** (ADR-014) in `StartMode::MigrateLegacyLocal`; the legacy FreertosPetService is no longer constructed by the board. Running both controllers simultaneously would be incorrect.

### Board migration (PetLedgerMigration)

`kid_app/pet_ledger_migration.h`, run on the Pet worker at every Start (before the ledger is loaded):

1. Read the "migrated" marker (`kid_pet_sync/migrated`, 32 bytes, own CRC) and probe the ledger. Marker + loadable ledger: done, nothing else is read or written (normal boot).
2. Otherwise read the legacy sources, never writing them: the kid_pet A/B checkpoint (PetPersistence) and the V3-06 board key `kid_parent/settings`. A corrupt parent key means the defaults (as the board already did); a corrupt kid_pet, or any read error, stops (retried on the next start). Old kid_bridge data is only noted in the marker flags.
3. Build the exact first ledger blob in RAM (`ImportLegacyLocal`: checkpoint with its replay window, generation continuing after the legacy one so new local ids never repeat, parent settings, empty memory and outbox) and write it to slot 0.
4. Verify: byte-exact read-back of slot 0, then an independent full `Load` (generation, parent settings, empty outbox).
5. Only then write the marker.

A power cut at any step converges: no ledger yet -> start over; our exact blob but no marker -> verified again and marked (`resumed`); a torn/unverified slot 0 with nothing in slot 1 -> rewritten from the still-intact legacy keys. Anything not explainable (a marker without a ledger, a ledger that differs from the legacy import without a marker, unknown data in slot 1, a corrupt marker) fails closed: nothing is erased, Pet mode stays off (`PET_RUNTIME_INIT_FAILED`) and the board logs `PET_LEDGER_MIGRATION outcome=failed failure=...`. The legacy keys stay as a rollback copy for an older firmware; changes made after the switch are not copied back.

The board has no server transport yet, so the ledger runs with `SetProjection(false)`: child actions commit without outbox records, so 16 offline feeds can never block the pet; the sequence cursor stays at 1. A future server must provision the device as new (and refuse if the marker says old kid_bridge data existed).

Board logs: `PET_LEDGER_MIGRATION` (worker), `PET_LEDGER_READY ok= migration= failure= flags= gen= fed= mode= mood= today= feeds= temperament=` (once per boot), `PET_LEDGER_STACK free=` (worker stack high-water mark), `PET_SLEEP_POWEROFF ... ledger_gen=`, `PET_LEDGER_PARENT result=`, `PET_MEMORY_COMMIT kind= src= result=`. Self-test `tools/selftest/v7_ledger_persist.pettest` (`# expect ledger_restored 1`) proves a feed survives a sleep power-off and an RTC-alarm boot.

Fresh or migrated initialization is always explicit. ImportLegacyForFreshServer requires the calling code to have verified all three conditions: a validated original kid_pet checkpoint, no unsent messages in the old kid_bridge outbox, and a newly provisioned authenticated backend device cursor at 0. It rejects reinitialization, preserving original replay IDs and advancing from legacy checkpoint generation. It deliberately cannot authenticate the remote cursor on its own.

Further device integration will route the existing FreeRTOS pet task through this single ledger for BOTH action commit and remote ACK dequeue, rather than using a postcommit callback to enqueue. Network transport must not mutate the ledger concurrently with the pet task. A network worker should post a copied ACK to the pet task's bounded inbox for durable commit, not call this ledger from a network callback.

When a device is offline for too long, 16 queued records may fill. Do not silently drop older records or tell a child an unavailable action succeeded. Future options include durable queue expansion, safe coalescing with proof of semantics, or an explicit offline limit policy.

## Tests

Host test: tests/cpp/test_pet_atomic_ledger.cc; runs in scripts/p2_test.sh. It covers fault injection before a commit, crash immediately after a feeding commit, reboot with an unacknowledged event, remote success with failed local ACK storage, repeated IDs, queue overflow, corruption recovery, fresh-server-only legacy import and transient voice normalization. tests/cpp/test_pet_ledger_migration.cc covers the board migration with simulated NVS (fresh device, legacy pet + parent carried over with replay ids, power cut before the marker, failed and torn ledger writes, read-back mismatch, foreign/unknown ledger data, marker problems, legacy read errors, later commits never rolled back, every cut converging to the same ledger, marker codec) and the local-only mode (40 feeds, empty outbox). ESP-IDF CI ensures the NVS store adapter compiles.

No physical firmware write or device-side network request is performed by this PR.


## Opt-in FreeRTOS single-owner task

FreertosAtomicPetService (header/source in firmware/components/kid_app) routes local button actions and external events through one persistent PetAtomicLedger, rather than through the old two-store PetEventExecutor. The alternate service explicitly selects ExistingOnly or FreshNoLegacy startup. FreshNoLegacy rejects any detected old kid_pet or kid_bridge checkpoint, and never silently erases either namespace.

An immutable validated device ID is bound during Start. The worker restores unacknowledged outbound head after reboot through a nonblocking AtomicPetOutboundCallback; the caller must **only copy** the record to its network task. Network submits the ACK back with SubmitAck, which queues a full copy of message_id, seq, accepted, duplicate and server version. Only the original Pet worker invokes Acknowledge and issues a new head after durable dequeue. If its NVS commit fails, the identical ACK remains pending for retry; on queue backpressure, the network worker must retry, never invent an ACK.

The new service must not run at the same time as the existing FreertosPetService. Do not switch a device with existing legacy state to FreshNoLegacy; use MigrateLegacyLocal (above) on a device without a server, and a separate supervised path that validates old outbox and server state before enabling projection on a synced device.

Besides pet events the service takes `PostMemory(MemoryEvent)` (id 0 = worker assigns a local id, day 0 = the board's RTC day via `SetDayProvider`) and `SaveParentSettings(settings, wait)` (blocks the caller, never the worker; not retried, so the parent menu reverts on failure; a timed-out save is abandoned under a gate, never committed later). `Settled()` counts every queued or in-progress message (incremented before the send), checked before PMIC power-off. `ParentNow()` / `MemoryNow()` / `TemperamentNow()` / `GenerationNow()` are copies of the last commit, safe from any task. Migration and the ledger load run on the worker's own 20 KiB stack while Start waits.
