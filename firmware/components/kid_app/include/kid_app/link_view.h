#pragma once

#include <cstdint>

namespace kid_app {

// What the parent entry's "connect to the home server" page shows
// (V5_V6_SERVER_PLAN §5.1). Written by the network task, read by the input
// task and drawn by SceneComposer, so it travels as one packed word:
// bits 0-3 state, bits 4-23 the pairing code as a number (0..999999, drawn
// with leading zeros), bits 24-31 zero. Header-only on purpose: the scene
// renderer needs it without linking the server protocol code.
enum class LinkState : uint8_t {
    Unpaired,     // no credentials yet: press to pair
    Connecting,   // asking the server for a code
    ShowCode,     // code on screen, the parent types it into the panel
    Saving,       // token received, storing and confirming it
    Paired,       // credentials stored and confirmed
    NoServer,     // no server address, or no pairing opened on the panel
    Failed,       // network or storage failure: press to try again
    NeedsRepair,  // the server refused the token (revoked): pair again
};

struct LinkView {
    LinkState state = LinkState::Unpaired;
    uint32_t code = 0;  // ShowCode only

    bool operator==(const LinkView& o) const { return state == o.state && code == o.code; }
    bool operator!=(const LinkView& o) const { return !(*this == o); }
};

inline uint32_t PackLinkView(const LinkView& view) {
    const uint32_t code = view.state == LinkState::ShowCode && view.code <= 999999 ? view.code : 0;
    return (static_cast<uint32_t>(view.state) & 0x0Fu) | (code << 4);
}

inline LinkView UnpackLinkView(uint32_t bits) {
    LinkView v{};
    const uint32_t state = bits & 0x0Fu;
    if (state > static_cast<uint32_t>(LinkState::NeedsRepair) || (bits >> 24)) return v;
    v.state = static_cast<LinkState>(state);
    const uint32_t code = (bits >> 4) & 0xFFFFFu;
    v.code = v.state == LinkState::ShowCode && code <= 999999 ? code : 0;
    return v;
}

}  // namespace kid_app
