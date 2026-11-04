#include "kid_app/parent_menu.h"
#include "kid_app/selftest_clock.h"

namespace kid_app {

namespace {
constexpr int kMinutesPerDay = 24 * 60;
}  // namespace

void ParentMenu::Open(const ParentSettings& current, uint32_t now_ms) {
    settings_ = ValidParentSettings(current) ? current : ParentSettings{};
    draft_ = settings_;
    open_ = true;
    editing_ = false;
    cursor_ = 0;
    last_input_ms_ = now_ms;
}

ParentView ParentMenu::View() const {
    ParentView view;
    view.cursor = cursor_;
    view.editing = editing_;
    view.settings = editing_ ? draft_ : settings_;
    return view;
}

void ParentMenu::Revert(const ParentSettings& previous) {
    settings_ = ValidParentSettings(previous) ? previous : ParentSettings{};
    draft_ = settings_;
    editing_ = false;
}

bool ParentMenu::Step(int direction) {
    uint16_t* minute = nullptr;
    uint8_t* volume = nullptr;
    uint8_t min_volume = 0;
    switch (Selected()) {
        case ParentItem::Wake: minute = &draft_.schedule.wake_min; break;
        case ParentItem::Evening: minute = &draft_.schedule.evening_min; break;
        case ParentItem::QuietStart: minute = &draft_.schedule.quiet_start_min; break;
        case ParentItem::QuietEnd: minute = &draft_.schedule.quiet_end_min; break;
        case ParentItem::Volume:
            volume = &draft_.volume_cap_pct;
            min_volume = ParentSettings::kMinVolumeCapPct;
            break;
        case ParentItem::QuietVolume: volume = &draft_.quiet_volume_pct; break;
        case ParentItem::Nap:
        case ParentItem::Motion: return false;  // switches flip on a press
    }
    if (volume) {
        const int next = *volume + direction * ParentSettings::kVolumeStepPct;
        if (next < min_volume || next > 100) return false;  // ends stay put
        *volume = static_cast<uint8_t>(next);
        return true;
    }
    // Times wrap around midnight. A step that would break the day plan
    // (e.g. evening running into quiet hours) is refused: the value stops
    // there and the parent moves the neighbouring time first.
    const uint16_t original = *minute;
    const int m = (original + direction * kTimeStepMin) % kMinutesPerDay;
    *minute = static_cast<uint16_t>(m < 0 ? m + kMinutesPerDay : m);
    if (RhythmScheduler::Valid(draft_.schedule)) return true;
    *minute = original;
    return false;
}

ParentMenuAction ParentMenu::OnInput(InputKind kind, uint32_t now_ms) {
    if (!open_) return ParentMenuAction::None;
    last_input_ms_ = now_ms;
    switch (kind) {
        case InputKind::DialLong:
            Close();
            return ParentMenuAction::Exit;
        case InputKind::DialUp:
        case InputKind::DialDown: {
            const bool up = kind == InputKind::DialUp;
            if (editing_) return Step(up ? +1 : -1) ? ParentMenuAction::Redraw
                                                    : ParentMenuAction::None;
            if (up ? cursor_ == 0 : cursor_ + 1 >= kParentItemCount) return ParentMenuAction::None;
            cursor_ = static_cast<uint8_t>(up ? cursor_ - 1 : cursor_ + 1);
            return ParentMenuAction::Redraw;
        }
        case InputKind::DialPress:
            if (editing_) {
                editing_ = false;
                if (draft_ == settings_) return ParentMenuAction::Redraw;
                settings_ = draft_;
                return ParentMenuAction::Saved;
            }
            draft_ = settings_;
            if (Selected() == ParentItem::Nap || Selected() == ParentItem::Motion) {
                if (Selected() == ParentItem::Nap)
                    draft_.schedule.nap_enabled = !draft_.schedule.nap_enabled;
                else
                    draft_.motion_enabled = !draft_.motion_enabled;
                if (!ValidParentSettings(draft_)) {  // nap outside this day plan
                    draft_ = settings_;
                    return ParentMenuAction::None;
                }
                settings_ = draft_;
                return ParentMenuAction::Saved;
            }
            editing_ = true;
            return ParentMenuAction::Redraw;
        case InputKind::Talk:
        case InputKind::BootLong:
        case InputKind::SleepRequest:
        case InputKind::ParentCombo:
            break;  // the menu ignores the other keys (still counts as input)
    }
    return ParentMenuAction::None;
}

ParentMenuAction ParentMenu::Tick(uint32_t now_ms) {
    if (!open_ || now_ms - last_input_ms_ < SelfTestClock::Scaled(kIdleExitMs))
        return ParentMenuAction::None;
    Close();
    return ParentMenuAction::Exit;
}

const char* ParentItemName(ParentItem item) {
    switch (item) {
        case ParentItem::Wake: return "wake";
        case ParentItem::Evening: return "evening";
        case ParentItem::QuietStart: return "quiet_start";
        case ParentItem::QuietEnd: return "quiet_end";
        case ParentItem::Nap: return "nap";
        case ParentItem::Volume: return "volume";
        case ParentItem::QuietVolume: return "quiet_volume";
        case ParentItem::Motion: return "motion";
    }
    return "unknown";
}

}  // namespace kid_app
