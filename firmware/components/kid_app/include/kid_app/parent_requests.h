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

    // Only kinds that need the parent; a known id is kept as it is.
    // False when full or the kind needs no confirmation.
    bool Offer(const DeviceCommand& command);
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
    // Every decided-but-unfinished request waits again (lost outcome).
    void ResetConfirming();
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
        bool confirming = false;
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
// two choices, check and cross. The highlight starts on the cross, the dial
// moves it, a press decides; a long press or kInputIdleMs without input
// leaves without deciding. Single owner (board input task).
class ConfirmDialog {
public:
    static constexpr uint32_t kIdleExitMs = 30000;

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
    DeviceCommand command_{};
};

}  // namespace kid_app
