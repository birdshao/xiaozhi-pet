# P3: atomic Pet action and outbound event checkpoint

## Problem closed by this module

In the previous P3 design the ESP32 committed a Pet action to kid_pet NVS and later queued the event separately to kid_bridge. A power loss between those two independent NVS writes could make feeding visible to the child but permanently absent from the server.

The new PetAtomicLedger, its codec, and NvsAtomicPetStore persist *both* in one 1,480-byte A/B NVS blob. An accepted action and its outbound record either both commit or neither commits. A remote ACK removes the outbox entry in the very same kind of durable aggregate write. No send occurs here; the Pet controller remains the owner of local effects.

- Namespace kid_pet_sync, keys joint_a / joint_b; neither old kid_pet nor kid_bridge is overwritten.
- Composite blob embeds the byte-exact verified PetCheckpointCodec, a 16-event FIFO, next server sequence, generation, CRC32 and schema magic. CRC detects corruption, not malicious alteration. Newest valid A/B slot is selected.
- Refuses unknown/corrupt-only NVS and failures reading either slot; never auto-erases.
- Applies feed/pickup/voice-start/voice-end/quiet-start/quiet-end in a single persistent blob. Story events remain local until book_id and approved-page context can be carried in the device event schema.
- If the 16-slot queue is full, projectable child actions are NOT committed or audibly acknowledged, preserving synchronization guarantees. Non-projectable story actions can still work locally. This capacity/buffering tradeoff requires a product policy decision before device rollout.
- Exact server message_id, seq and positive version must be validated before durable ACK dequeue. Lost ACK can safely replay the *same* original event.
- On restart a transitional Listening/Speaking mode is normalized to Idle without corrupting the saved outbox.

## Operational status and migration

This is a newly implemented core, NVS adapter, and an **opt-in FreeRTOS event service**, not yet activated by the Waveshare board integration. Legacy FreertosPetService continues using the old kid_pet store until the event task is switched deliberately to a single atomic ledger owner. Running both controllers simultaneously would be incorrect.

Fresh or migrated initialization is always explicit. ImportLegacyForFreshServer requires the calling code to have verified all three conditions: a validated original kid_pet checkpoint, no unsent messages in the old kid_bridge outbox, and a newly provisioned authenticated backend device cursor at 0. It rejects reinitialization, preserving original replay IDs and advancing from legacy checkpoint generation. It deliberately cannot authenticate the remote cursor on its own.

Further device integration will route the existing FreeRTOS pet task through this single ledger for BOTH action commit and remote ACK dequeue, rather than using a postcommit callback to enqueue. Network transport must not mutate the ledger concurrently with the pet task. A network worker should post a copied ACK to the pet task's bounded inbox for durable commit, not call this ledger from a network callback.

When a device is offline for too long, 16 queued records may fill. Do not silently drop older records or tell a child an unavailable action succeeded. Future options include durable queue expansion, safe coalescing with proof of semantics, or an explicit offline limit policy.

## Tests

Host test: tests/cpp/test_pet_atomic_ledger.cc; runs in scripts/p2_test.sh. It covers fault injection before a commit, crash immediately after a feeding commit, reboot with an unacknowledged event, remote success with failed local ACK storage, repeated IDs, queue overflow, corruption recovery, fresh-server-only legacy import and transient voice normalization. ESP-IDF CI ensures the NVS store adapter compiles.

No physical firmware write or device-side network request is performed by this PR.


## Opt-in FreeRTOS single-owner task

FreertosAtomicPetService (header/source in firmware/components/kid_app) routes local button actions and external events through one persistent PetAtomicLedger, rather than through the old two-store PetEventExecutor. The alternate service explicitly selects ExistingOnly or FreshNoLegacy startup. FreshNoLegacy rejects any detected old kid_pet or kid_bridge checkpoint, and never silently erases either namespace.

An immutable validated device ID is bound during Start. The worker restores unacknowledged outbound head after reboot through a nonblocking AtomicPetOutboundCallback; the caller must **only copy** the record to its network task. Network submits the ACK back with SubmitAck, which queues a full copy of message_id, seq, accepted, duplicate and server version. Only the original Pet worker invokes Acknowledge and issues a new head after durable dequeue. If its NVS commit fails, the identical ACK remains pending for retry; on queue backpressure, the network worker must retry, never invent an ACK.

The new service must not run at the same time as the existing FreertosPetService. Neither a real HTTP transport nor the Waveshare board's choice between old and new service is changed in this PR. Do not switch a device with existing legacy state to FreshNoLegacy; a separate supervised migration path must validate old outbox and server state first.
