#include "kid_app/parent_menu.h"
#include "kid_app/selftest_clock.h"

namespace kid_app {

namespace {
constexpr int kMinutesPerDay = 24 * 60;
constexpr uint8_t kHabits = 8;  // Habit presets (care_store.h)
// Days per month; February allows the 29th (a leap-year birthday).
uint8_t DaysIn(uint8_t month) {
    static const uint8_t kDays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month >= 1 && month <= 12 ? kDays[month - 1] : 0;
}
ParentCare Sanitize(const ParentCare& c) {
    ParentCare out = c;
    if (out.habit > kHabits) out.habit = 0;
    if (!DaysIn(out.birth_month) || out.birth_day < 1 || out.birth_day > DaysIn(out.birth_month))
        out.birth_month = out.birth_day = 0;
    return out;
}
}  // namespace

void ParentMenu::Open(const ParentSettings& current, uint32_t now_ms, const ParentCare& care) {
    settings_ = ValidParentSettings(current) ? current : ParentSettings{};
    draft_ = settings_;
    care_ = Sanitize(care);
    care_draft_ = care_;
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
    view.care = editing_ ? care_draft_ : care_;
    return view;
}

void ParentMenu::RevertCare(const ParentCare& previous) {
    care_ = Sanitize(previous);
    care_draft_ = care_;
    editing_ = false;
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
        case ParentItem::Motion:
        case ParentItem::Server: return false;  // switches flip on a press; Server opens a page
        case ParentItem::Habit: {  // none, then the presets; the ends stay put
            const int next = care_draft_.habit + direction;
            if (next < 0 || next > kHabits) return false;
            care_draft_.habit = static_cast<uint8_t>(next);
            return true;
        }
        case ParentItem::BirthMonth: {  // 0 = not set; a day is kept valid
            const int next = care_draft_.birth_month + direction;
            if (next < 0 || next > 12) return false;
            care_draft_.birth_month = static_cast<uint8_t>(next);
            if (!next) care_draft_.birth_day = 0;
            else if (!care_draft_.birth_day) care_draft_.birth_day = 1;
            else if (care_draft_.birth_day > DaysIn(care_draft_.birth_month))
                care_draft_.birth_day = DaysIn(care_draft_.birth_month);
            return true;
        }
        case ParentItem::BirthDay: {  // needs a month first
            if (!care_draft_.birth_month) return false;
            const int next = care_draft_.birth_day + direction;
            if (next < 1 || next > DaysIn(care_draft_.birth_month)) return false;
            care_draft_.birth_day = static_cast<uint8_t>(next);
            return true;
        }
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
                if (care_draft_ != care_) {  // V8 page 2
                    care_ = care_draft_;
                    return ParentMenuAction::SavedCare;
                }
                care_draft_ = care_;
                if (draft_ == settings_) return ParentMenuAction::Redraw;
                settings_ = draft_;
                return ParentMenuAction::Saved;
            }
            if (Selected() == ParentItem::Server) return ParentMenuAction::OpenLink;
            draft_ = settings_;
            care_draft_ = care_;
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
        case ParentItem::Server: return "server";
        case ParentItem::Habit: return "habit";
        case ParentItem::BirthMonth: return "birth_month";
        case ParentItem::BirthDay: return "birth_day";
    }
    return "unknown";
}

}  // namespace kid_app
