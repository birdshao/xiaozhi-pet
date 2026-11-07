#pragma once

#include "kid_app/input_hub.h"
#include "kid_app/parent_menu.h"
#include "kid_app/server_sync.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// D5 (V5_V6_SERVER_PLAN §5.3): a memory clear or a temperament change from
// the parent panel waits on the device until the parent confirms it in the
// device parent entry. Waiting requests live in RAM only and are reported
// in the next snapshot's awaiting_confirm; nothing touches the ledger until
// the parent confirms (then the Pet task commits with MemorySource::Parent).

// Requests waiting for the parent. Shared by the network task (offers,
// expiry, awaiting list) and the input task (shows, resolves): the owner
// wraps every call in one mutex. Pure data, no locking of its own.
class ParentRequestQueue {
public:
    static constexpr size_t kCapacity = kMaxAwaitingConfirm;

    enum class OfferResult : uint8_t {
        Added,      // queued
        Replaced,   // queued; an older request of the same class was dropped
        Known,      // already queued (a redelivery)
        Stale,      // an equal-or-newer one is waiting: drop this command
        Refused,    // no confirmation needed, or the queue is full
    };
    // Only kinds that need the parent. A newer request of an ordered class
    // (higher seq, or the same seq arriving later) replaces the waiting one
    // (not one already being committed): the parent can never confirm the
    // old value after the new one. *replaced_id gets the dropped id.
    OfferResult Offer(const DeviceCommand& command, uint64_t* replaced_id = nullptr);
    // Drops undecided requests of `kind` whose seq is below `applied`;
    // returns their ids (answered rejected by the caller).
    size_t DropStale(CommandKind kind, uint64_t applied, uint64_t* out, size_t capacity);
    bool Contains(uint64_t id) const;
    bool Find(uint64_t id, DeviceCommand* out, bool* confirming) const;
    size_t Count() const { return count_; }
    // Not yet decided (what the home hint and the parent page show).
    size_t Waiting() const;
    // Oldest undecided request.
    bool Next(DeviceCommand* out) const;
    // The parent confirmed: the commit is in flight on the Pet task (a
    // storage failure puts it back to waiting).
    bool SetConfirming(uint64_t id, bool confirming);
    // The parent refused: hidden until the network task has stored it.
    bool SetRejecting(uint64_t id);
    // Requests the parent confirmed whose outcome has not come back yet
    // (to be posted to the Pet task again: the ledger dedups by id).
    size_t Confirming(DeviceCommand* out, size_t capacity) const;
    bool Remove(uint64_t id);
    size_t Ids(uint64_t* out, size_t capacity) const;
    // Removes undecided requests past expires_at (clock known) and returns
    // their ids for "expired" receipts.
    size_t RemoveExpired(bool clock_known, int64_t now_utc, uint64_t* out, size_t capacity);
    // Changes whenever the set of ids or what the parent sees changes.
    uint32_t Revision() const { return revision_; }

private:
    struct Entry {
        DeviceCommand command{};
        bool confirming = false;  // approved, commit in flight
        bool rejecting = false;   // refused, being stored
        bool Busy() const { return confirming || rejecting; }
    };
    Entry entries_[kCapacity]{};
    uint8_t count_ = 0;
    uint32_t revision_ = 0;
};

enum class ConfirmAction : uint8_t {
    None,     // nothing changed on screen
    Redraw,   // the highlight moved
    Confirm,  // the parent pressed on the check mark
    Reject,   // the parent pressed on the cross
    Exit,     // long press or idle: decide later (the request keeps waiting)
};

// The "待确认请求" page: what is asked (wipe memory / new temperament) and
// two choices, check and cross. The highlight starts on the cross. Dial up
// selects the check, dial down the cross (a direction, never a toggle: what
// the parent turned to is what is chosen). A press decides, but only once
// kSettleMs have passed since the highlight last moved (the e-paper needs
// that long to show it), so a press can never decide something the parent
// has not seen. A long press or kIdleExitMs without input leaves without
// deciding. Single owner (board input task).
class ConfirmDialog {
public:
    static constexpr uint32_t kIdleExitMs = 30000;
    static constexpr uint32_t kSettleMs = 3000;

    void Open(const DeviceCommand& command, uint8_t more, uint32_t now_ms);
    void Close() { open_ = false; }
    bool IsOpen() const { return open_; }
    ConfirmAction OnInput(InputKind kind, uint32_t now_ms);
    ConfirmAction Tick(uint32_t now_ms);
    const DeviceCommand& Command() const { return command_; }
    RequestView View() const;

private:
    bool open_ = false;
    bool confirm_ = false;
    uint8_t more_ = 0;
    uint32_t last_input_ms_ = 0;
    uint32_t changed_ms_ = 0;  // when the highlight last moved (or the page opened)
    DeviceCommand command_{};
};

}  // namespace kid_app
