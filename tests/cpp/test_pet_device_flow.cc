#include "kid_app/pet_controller.h"
#include "kid_app/pet_event_executor.h"
#include "kid_app/pet_scene_presenter.h"
#include "kid_app/pet_refresh_gate.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

class TestNvs : public PetBlobStore {
public:
    BlobReadStatus Read(unsigned slot, PetCheckpointCodec::Blob* out) override {
        if (slot >= 2 || !out) return BlobReadStatus::Error;
        if (!valid[slot]) return BlobReadStatus::NotFound;
        *out = slots[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const PetCheckpointCodec::Blob& blob) override {
        if (slot >= 2 || blocked) return false;
        slots[slot] = blob;
        valid[slot] = true;
        return true;
    }
    bool blocked = false;
    bool valid[2]{};
    PetCheckpointCodec::Blob slots[2]{};
};

int main() {
    TestNvs store;
    PetController pet;
    PetPersistence persist(store);
    PetEventExecutor executor(pet, persist);
    PetRefreshGate epd(5000);

    // The first screen paint is independent of NVS event delivery.
    SceneModel first = PetScenePresenter::Present(pet.Snapshot());
    assert(epd.Request(first));
    SceneModel paint{};
    assert(epd.Acquire(0, &paint));
    assert(paint.kind == SceneKind::Home);
    assert(paint.emotion == Emotion::Neutral);
    assert(epd.Complete(true, 1800));

    PetEffects effects{};
    // A failed NVS transaction must never trigger a pet scene update.
    store.blocked = true;
    assert(executor.ApplyDurably({1, PetAction::Pickup}, &effects) ==
           CommitResult::StorageError);
    assert(!effects.repaint);
    assert(pet.Snapshot().mood == PetMood::Neutral);
    assert(!epd.Request(PetScenePresenter::Present(pet.Snapshot())));

    store.blocked = false;
    assert(executor.ApplyDurably({1, PetAction::Pickup}, &effects) ==
           CommitResult::Committed);
    assert(effects.repaint);
    assert(epd.Request(PetScenePresenter::Present(pet.Snapshot())));
    assert(!epd.Acquire(6799, &paint));
    assert(epd.Acquire(6800, &paint));
    assert(paint.emotion == Emotion::Happy);

    // While the panel is physically BUSY, the child can open a book and
    // flip pages. The display queue should show only the latest page.
    assert(executor.ApplyDurably({2, PetAction::StartStory}, &effects) ==
           CommitResult::Committed);
    assert(epd.Request(PetScenePresenter::Present(pet.Snapshot())));
    assert(executor.ApplyDurably({3, PetAction::NextPage}, &effects) ==
           CommitResult::Committed);
    assert(epd.Request(PetScenePresenter::Present(pet.Snapshot())));
    assert(epd.Complete(true, 9000));
    assert(!epd.Acquire(13999, &paint));
    assert(epd.Acquire(14000, &paint));
    assert(paint.kind == SceneKind::Reader);
    assert(paint.book_page == 2);
    assert(epd.Complete(true, 15000));

    assert(executor.ApplyDurably({4, PetAction::QuietBegin}, &effects) ==
           CommitResult::Committed);
    assert(effects.repaint);
    assert(epd.Request(PetScenePresenter::Present(pet.Snapshot())));
    assert(epd.Acquire(20000, &paint));
    assert(paint.kind == SceneKind::Home);
    assert(paint.emotion == Emotion::Sleepy);
    assert(epd.Complete(true, 20500));

    // Restarted state must render the persisted sleepy scene; local replay
    // of already committed quiet event must produce no effects.
    PetController after_reboot;
    PetPersistence restored(store);
    assert(restored.Load(&after_reboot) == PetLoadResult::Restored);
    assert(after_reboot.Snapshot().quiet);
    PetEventExecutor rebooted_executor(after_reboot, restored);
    assert(rebooted_executor.ApplyDurably({4, PetAction::QuietBegin}, &effects) ==
           CommitResult::Duplicate);
    assert(!effects.repaint);
    epd.Reset();  // Original LVGL UI overwrote the pet frame when exiting.
    assert(epd.Request(PetScenePresenter::Present(after_reboot.Snapshot())));
    assert(epd.Acquire(21000, &paint));
    assert(paint.emotion == Emotion::Sleepy);

    std::cout << "PASS: durable pet event -> SceneModel -> EPD gate, reboot and replay\n";
}
