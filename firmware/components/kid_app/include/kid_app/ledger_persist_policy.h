#pragma once

#include "kid_app/pet_controller.h"
#include "kid_app/pet_event_executor.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// What the atomic Pet worker does when a commit keeps failing (NVS full,
// flash error). The message stays at the head and is retried, but only a
// bounded number of times: one unsavable action must never block every
// later one (the dial would be dead until a reboot). A dropped action was
// never committed, so the child got no success feedback for it.
struct PersistRetry {
    static constexpr uint32_t kAttempts = 8;  // x 750 ms: about 6 s

    uint32_t failures = 0;

    // True when the attempt about to run is the last one for this message.
    bool LastAttempt() const { return failures + 1 >= kAttempts; }
    // After a failed attempt: true = retry the same message, false = drop it.
    bool OnFailure() { return ++failures < kAttempts; }
    void OnDone() { failures = 0; }
};

// "Home" from the reader could not be saved: the board still shows the home
// rail (UI only), so a long press can never leave the child stuck in Story.
constexpr bool StoryEscapeNeeded(PetAction action, CommitResult result) {
    return action == PetAction::StopStory && result == CommitResult::StorageError;
}

// Restored Story mode without a readable book (no card, empty or broken
// pack): go home at start instead of opening an empty reader.
constexpr bool RestoredStoryNeedsHome(PetMode mode, bool book_readable) {
    return mode == PetMode::Story && !book_readable;
}

// NVS entries (32 bytes each) one blob value holds when it fits one chunk:
// the blob index, the chunk header and the data. Rewriting a key needs this
// many free entries while the old value still exists.
constexpr size_t NvsBlobEntries(size_t bytes) { return 2 + (bytes + 31) / 32; }

}  // namespace kid_app
