#include "kid_app/parent_requests.h"

#include "kid_app/selftest_clock.h"

namespace kid_app {

bool ParentRequestQueue::Offer(const DeviceCommand& command) {
    if (!command.id || !NeedsParentConfirm(command.kind)) return false;
    if (Contains(command.id)) return true;
    if (count_ >= kCapacity) return false;
    entries_[count_++] = Entry{command, false};
    ++revision_;
    return true;
}

bool ParentRequestQueue::Contains(uint64_t id) const {
    for (uint8_t i = 0; i < count_; ++i) {
        if (entries_[i].command.id == id) return true;
    }
    return false;
}

bool ParentRequestQueue::Find(uint64_t id, DeviceCommand* out, bool* confirming) const {
    for (uint8_t i = 0; i < count_; ++i) {
        if (entries_[i].command.id != id) continue;
        if (out) *out = entries_[i].command;
        if (confirming) *confirming = entries_[i].confirming;
        return true;
    }
    return false;
}

void ParentRequestQueue::ResetConfirming() {
    for (uint8_t i = 0; i < count_; ++i) {
        if (!entries_[i].confirming) continue;
        entries_[i].confirming = false;
        ++revision_;
    }
}

size_t ParentRequestQueue::Waiting() const {
    size_t n = 0;
    for (uint8_t i = 0; i < count_; ++i) n += entries_[i].confirming ? 0 : 1;
    return n;
}

bool ParentRequestQueue::Next(DeviceCommand* out) const {
    for (uint8_t i = 0; i < count_; ++i) {
        if (entries_[i].confirming) continue;
        if (out) *out = entries_[i].command;
        return true;
    }
    return false;
}

bool ParentRequestQueue::SetConfirming(uint64_t id, bool confirming) {
    for (uint8_t i = 0; i < count_; ++i) {
        if (entries_[i].command.id != id) continue;
        if (entries_[i].confirming != confirming) {
            entries_[i].confirming = confirming;
            ++revision_;
        }
        return true;
    }
    return false;
}

bool ParentRequestQueue::Remove(uint64_t id) {
    for (uint8_t i = 0; i < count_; ++i) {
        if (entries_[i].command.id != id) continue;
        for (uint8_t j = i; j + 1 < count_; ++j) entries_[j] = entries_[j + 1];
        entries_[--count_] = Entry{};
        ++revision_;
        return true;
    }
    return false;
}

size_t ParentRequestQueue::Ids(uint64_t* out, size_t capacity) const {
    size_t n = 0;
    for (; out && n < count_ && n < capacity; ++n) out[n] = entries_[n].command.id;
    return n;
}

size_t ParentRequestQueue::RemoveExpired(bool clock_known, int64_t now_utc, uint64_t* out,
                                         size_t capacity) {
    size_t n = 0;
    for (uint8_t i = 0; i < count_ && n < capacity;) {
        const Entry& e = entries_[i];
        if (!e.confirming && CommandExpired(e.command, clock_known, now_utc)) {
            if (out) out[n] = e.command.id;
            ++n;
            Remove(e.command.id);  // shifts the next entry into i
        } else {
            ++i;
        }
    }
    return n;
}

void ConfirmDialog::Open(const DeviceCommand& command, uint8_t more, uint32_t now_ms) {
    open_ = true;
    confirm_ = false;  // a stray press refuses
    more_ = more;
    last_input_ms_ = now_ms;
    command_ = command;
}

ConfirmAction ConfirmDialog::OnInput(InputKind kind, uint32_t now_ms) {
    if (!open_) return ConfirmAction::None;
    last_input_ms_ = now_ms;
    switch (kind) {
        case InputKind::DialUp:
        case InputKind::DialDown:
            confirm_ = !confirm_;
            return ConfirmAction::Redraw;
        case InputKind::DialPress:
            open_ = false;
            return confirm_ ? ConfirmAction::Confirm : ConfirmAction::Reject;
        case InputKind::DialLong:
            open_ = false;
            return ConfirmAction::Exit;
        case InputKind::Talk:
        case InputKind::BootLong:
        case InputKind::SleepRequest:
        case InputKind::ParentCombo:
            break;  // ignored (still counts as input)
    }
    return ConfirmAction::None;
}

ConfirmAction ConfirmDialog::Tick(uint32_t now_ms) {
    if (!open_ || now_ms - last_input_ms_ < SelfTestClock::Scaled(kIdleExitMs))
        return ConfirmAction::None;
    open_ = false;
    return ConfirmAction::Exit;
}

RequestView ConfirmDialog::View() const {
    RequestView v{};
    v.kind = command_.kind == CommandKind::SetTemperament ? RequestKind::Temperament
                                                          : RequestKind::MemoryClear;
    v.temperament = static_cast<uint8_t>(command_.temperament);
    v.confirm = confirm_;
    v.more = more_;
    return v;
}

}  // namespace kid_app
