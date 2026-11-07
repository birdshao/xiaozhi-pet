#pragma once

#include "kid_app/pet_atomic_ledger.h"
#include "kid_app/server_sync.h"

#include <cstdint>

namespace kid_app {

// What happened to a server command on the Pet task (the only task that
// may touch PetAtomicLedger). The command id is the ledger event id, so a
// command applied once is a replay-window duplicate forever after (§5.3).
enum class CommandOutcome : uint8_t {
    Applied,         // committed now
    AlreadyApplied,  // its id is already in the replay window
    NeedsConfirm,    // D5: waits for the parent in the device parent entry
    Rejected,        // unsupported kind, bad payload, refused by MemoryStore
    StorageError,    // nothing committed; the command can be tried again
};
const char* CommandOutcomeName(CommandOutcome outcome);

// Pet task only. parent_confirmed: the parent pressed "confirm" on the
// request page (the commit then has MemorySource::Parent, which MemoryStore
// requires for a wipe or a temperament). day: local civil day for the
// memory event (0 = unknown). Expiry is the caller's check. min_seq: the
// highest seq already applied for this command's ordered class; a command
// below it is rejected here too (defence in depth: a stale request that was
// already waiting on the Pet task must not put an old value back).
CommandOutcome ApplyServerCommand(PetAtomicLedger& ledger, const DeviceCommand& command,
                                  bool parent_confirmed, uint32_t day, uint64_t min_seq = 0);

// Receipt for a final outcome; false for NeedsConfirm / StorageError
// (nothing to report yet).
bool ReceiptForOutcome(CommandOutcome outcome, ReceiptResult* out);

}  // namespace kid_app
