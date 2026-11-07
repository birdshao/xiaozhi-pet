#pragma once

#include "kid_app/input_hub.h"
#include "kid_app/link_view.h"
#include "kid_app/parent_settings.h"

#include <cstdint>

namespace kid_app {

// Rows of the on-device parent menu, top to bottom. Times are edited in
// kTimeStepMin steps, volumes in ParentSettings::kVolumeStepPct steps; Nap
// and Motion are switches. Server (V5_V6 §5.1, last row) opens the
// "connect to the home server" page instead of editing a value.
enum class ParentItem : uint8_t {
    Wake, Evening, QuietStart, QuietEnd, Nap, Volume, QuietVolume, Motion, Server
};
constexpr uint8_t kParentItemCount = 9;

// Which page of the parent entry SceneKind::Parent draws: the settings
// menu, a request from the parent panel waiting for confirmation (D5), or
// the server pairing page.
enum class ParentPage : uint8_t { Menu, Request, Link };

// D5 request page: what the parent panel asks for and the highlighted
// choice (check = confirm, cross = reject).
enum class RequestKind : uint8_t { MemoryClear, Temperament };
struct RequestView {
    RequestKind kind = RequestKind::MemoryClear;
    uint8_t temperament = 0;  // Temperament value (Temperament requests)
    bool confirm = false;     // highlight on the check mark (else the cross)
    uint8_t more = 0;         // further requests waiting after this one

    bool operator==(const RequestView& o) const {
        return kind == o.kind && temperament == o.temperament && confirm == o.confirm &&
               more == o.more;
    }
    bool operator!=(const RequestView& o) const { return !(*this == o); }
};

// What SceneKind::Parent draws: the highlighted row, whether it is being
// edited and the values shown (the draft while editing); on the other
// pages the request or the server link. `link` also marks the Server row.
struct ParentView {
    uint8_t cursor = 0;
    bool editing = false;
    ParentSettings settings{};
    ParentPage page = ParentPage::Menu;
    RequestView request{};
    LinkView link{};

    bool operator==(const ParentView& o) const {
        return cursor == o.cursor && editing == o.editing && settings == o.settings &&
               page == o.page && request == o.request && link == o.link;
    }
    bool operator!=(const ParentView& o) const { return !(*this == o); }
};

enum class ParentMenuAction : uint8_t {
    None,    // nothing changed on screen
    Redraw,  // cursor or draft moved: draw View()
    Saved,   // Settings() changed: persist and apply them, then draw View()
    Exit,    // back to the pet (long press or 30 s without input)
    OpenLink,  // press on the Server row: show the server pairing page
};

// PRD v0.2 §4: entered with BOOT + dial press held 5 s (InputHub
// ParentCombo); the parent works the dial on screen. Dial up/down moves
// the highlight; a press starts editing a value (or flips a switch); while
// editing up/down change the value and a press saves it. A long press
// leaves at once (an unsaved draft is dropped), as does 30 s without input.
// Every reachable value keeps the whole plan valid: a step that would break
// RhythmScheduler::Valid is refused (the value stops there).
//
// Pure UI state: opening, moving and leaving never touch the pet. Single
// owner (board input task); uint32 ms clock, wrap-safe.
class ParentMenu {
public:
    static constexpr uint32_t kIdleExitMs = 30000;
    static constexpr uint16_t kTimeStepMin = 15;

    void Open(const ParentSettings& current, uint32_t now_ms);
    void Close() { open_ = false; editing_ = false; }
    // Input elsewhere in the parent entry (request / server pages) keeps
    // the menu from timing out underneath them.
    void Touch(uint32_t now_ms) { last_input_ms_ = now_ms; }
    bool IsOpen() const { return open_; }
    bool Editing() const { return open_ && editing_; }

    ParentMenuAction OnInput(InputKind kind, uint32_t now_ms);
    // Exit once kIdleExitMs pass without input.
    ParentMenuAction Tick(uint32_t now_ms);
    // Persisting failed: show `previous` again (stays open, not editing).
    void Revert(const ParentSettings& previous);

    // Saved values (what the device should run with).
    const ParentSettings& Settings() const { return settings_; }
    ParentItem Selected() const { return static_cast<ParentItem>(cursor_); }
    ParentView View() const;

private:
    // Moves the draft value of the selected item one step; false at the end
    // of its range or where the day plan would become invalid.
    bool Step(int direction);

    bool open_ = false;
    bool editing_ = false;
    uint8_t cursor_ = 0;
    uint32_t last_input_ms_ = 0;
    ParentSettings settings_{};
    ParentSettings draft_{};
};

const char* ParentItemName(ParentItem item);

}  // namespace kid_app
