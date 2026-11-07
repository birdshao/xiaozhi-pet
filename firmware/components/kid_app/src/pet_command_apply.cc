#include "kid_app/pet_command_apply.h"

#include "kid_app/pet_local_event_id.h"

namespace kid_app {

const char* CommandOutcomeName(CommandOutcome outcome) {
    switch (outcome) {
        case CommandOutcome::Applied: return "applied";
        case CommandOutcome::AlreadyApplied: return "already_applied";
        case CommandOutcome::NeedsConfirm: return "needs_confirm";
        case CommandOutcome::Rejected: return "rejected";
        case CommandOutcome::StorageError: return "storage";
    }
    return "unknown";
}

CommandOutcome ApplyServerCommand(PetAtomicLedger& ledger, const DeviceCommand& command,
                                  bool parent_confirmed, uint32_t day) {
    // Server ids live in the external id space (bit 63 clear, below 2^53).
    if (!IsExternalEventId(command.id) || command.id > kMaxCommandId)
        return CommandOutcome::Rejected;
    // Checked before anything else: a redelivered command that already ran
    // is reported applied, without asking the parent a second time.
    if (ledger.Seen(command.id)) return CommandOutcome::AlreadyApplied;
    if (!SupportedCommand(command.kind) || !command.payload_ok) return CommandOutcome::Rejected;
    // D5 by kind, never by what the server says.
    if (NeedsParentConfirm(command.kind) && !parent_confirmed) return CommandOutcome::NeedsConfirm;

    MemoryEvent event{};
    event.id = command.id;
    event.source = MemorySource::Parent;
    event.day = day;
    switch (command.kind) {
        case CommandKind::MemoryClear:
            event.kind = MemoryEventKind::MemoryCleared;
            break;
        case CommandKind::SetTemperament:
            event.kind = MemoryEventKind::TemperamentSet;
            event.option = static_cast<uint16_t>(command.temperament);
            break;
        default:
            return CommandOutcome::Rejected;
    }
    switch (ledger.RecordMemory(event)) {
        case CommitResult::Committed: return CommandOutcome::Applied;
        case CommitResult::Duplicate: return CommandOutcome::AlreadyApplied;
        case CommitResult::Rejected: return CommandOutcome::Rejected;
        case CommitResult::StorageError: return CommandOutcome::StorageError;
    }
    return CommandOutcome::StorageError;
}

bool ReceiptForOutcome(CommandOutcome outcome, ReceiptResult* out) {
    if (!out) return false;
    switch (outcome) {
        case CommandOutcome::Applied:
        case CommandOutcome::AlreadyApplied:
            *out = ReceiptResult::Applied;
            return true;
        case CommandOutcome::Rejected:
            *out = ReceiptResult::Rejected;
            return true;
        case CommandOutcome::NeedsConfirm:
        case CommandOutcome::StorageError:
            break;
    }
    return false;
}

}  // namespace kid_app
