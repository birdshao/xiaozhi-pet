#include "kid_app/pet_atomic_ledger.h"
#include "kid_app/pet_local_event_id.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>

using namespace kid_app;
using Ledger = PetAtomicLedger;
using Bridge = PetDeviceBridge;

class FakeNvs : public Ledger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out) override {
        if (read_error) return BlobReadStatus::Error;
        if (slot>1 || !out) return BlobReadStatus::Error;
        if (!present[slot]) return BlobReadStatus::NotFound;
        *out=slots[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const Blob& data) override {
        if (write_error || slot>1) return false;
        slots[slot]=data;
        present[slot]=true;
        return true;
    }
    bool present[2]{};
    bool read_error=false;
    bool write_error=false;
    Blob slots[2]{};
};

Ledger::AckResult Confirm(Ledger& l, const Bridge::Record& front,
                          uint64_t server_version = 1, bool accepted = true) {
    char id[96]{};
    const int length=std::snprintf(id,sizeof(id),"pet-001:%016llx",
                                  static_cast<unsigned long long>(front.event_id));
    assert(length>0 && static_cast<size_t>(length)<sizeof(id));
    return l.Acknowledge({id,front.sequence,server_version,accepted,false},"pet-001");
}

void TestAtomicFeedCommitCrashAndAck() {
    FakeNvs disk;
    Ledger first(disk);
    assert(first.Load()==Ledger::LoadResult::Empty);
    PetEffects effects{};
    const auto id=NextLocalEventId(1);
    assert(first.ApplyDurably({id,PetAction::Feed},&effects)==CommitResult::StorageError);
    assert(first.InitializeFresh());
    assert(first.Generation()==1);

    disk.write_error=true;
    assert(first.ApplyDurably({id,PetAction::Feed},&effects)==CommitResult::StorageError);
    assert(!effects.persist && !effects.play_positive_sound && !effects.repaint);
    assert(first.Snapshot().interaction_count==0);
    assert(first.PendingCount()==0 && first.NextSequence()==1);

    disk.write_error=false;
    assert(first.ApplyDurably({id,PetAction::Feed},&effects)==CommitResult::Committed);
    assert(effects.play_positive_sound && effects.persist);
    assert(first.Snapshot().interaction_count==1);
    Bridge::Record outbound{};
    assert(first.Peek(&outbound));
    assert(outbound.event_id==id && outbound.sequence==1 &&
           outbound.action==PetAction::Feed);

    // Power cut between local effect & outbound HTTP: BOTH are in one blob.
    Ledger after_reboot(disk);
    assert(after_reboot.Load()==Ledger::LoadResult::Ready);
    assert(after_reboot.Snapshot().interaction_count==1);
    assert(after_reboot.Peek(&outbound));
    assert(outbound.event_id==id && outbound.sequence==1);
    assert(after_reboot.ApplyDurably({id,PetAction::Feed},&effects)==CommitResult::Duplicate);
    assert(!effects.play_positive_sound && !effects.persist);
    assert(after_reboot.Snapshot().interaction_count==1);

    // Wrong/stale per-event reply cannot remove the outbound notification.
    assert(after_reboot.Acknowledge({"pet-OTHER:8000000000000002",1,1,true,false},
                                    "pet-001")==Ledger::AckResult::Mismatch);
    assert(after_reboot.Acknowledge({"pet-001:8000000000000002",2,1,true,false},
                                    "pet-001")==Ledger::AckResult::Mismatch);
    assert(after_reboot.PendingCount()==1);
    assert(Confirm(after_reboot,outbound,0)==Ledger::AckResult::Rejected);
    assert(Confirm(after_reboot,outbound,1,false)==Ledger::AckResult::Rejected);

    // Network succeeded, but device crashed before ACK NVS commit: replay
    // same durable event ID and server replies DUPLICATE with old version.
    disk.write_error=true;
    assert(Confirm(after_reboot,outbound)==Ledger::AckResult::StorageError);
    disk.write_error=false;
    Ledger after_lost_ack(disk);
    assert(after_lost_ack.Load()==Ledger::LoadResult::Ready);
    assert(after_lost_ack.PendingCount()==1);
    assert(after_lost_ack.Snapshot().interaction_count==1);
    assert(after_lost_ack.Peek(&outbound));
    assert(Confirm(after_lost_ack,outbound)==Ledger::AckResult::Confirmed);
    assert(after_lost_ack.PendingCount()==0);
    assert(after_lost_ack.NextSequence()==2);

    Ledger after_final_reboot(disk);
    assert(after_final_reboot.Load()==Ledger::LoadResult::Ready);
    assert(after_final_reboot.Snapshot().interaction_count==1);
    assert(after_final_reboot.PendingCount()==0);
    assert(after_final_reboot.NextSequence()==2);
}

void TestCapacityAndNoLostUpdates() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load()==Ledger::LoadResult::Empty);
    assert(l.InitializeFresh());

    PetEffects fx{};
    for (size_t i=0;i<Bridge::kCapacity;++i) {
        assert(l.ApplyDurably({100+i,PetAction::Feed},&fx)==CommitResult::Committed);
    }
    const auto before=l.Generation();
    assert(l.PendingCount()==Bridge::kCapacity);
    assert(l.Snapshot().interaction_count==Bridge::kCapacity);
    assert(l.ApplyDurably({1000,PetAction::Feed},&fx)==CommitResult::StorageError);
    assert(l.Generation()==before);
    assert(l.Snapshot().interaction_count==Bridge::kCapacity);
    assert(!fx.persist && !fx.play_positive_sound);

    // Unsynced book turns have no book ID in PetEvent; they are committed
    // to pet state but NOT falsely projected to approved Python book pages.
    assert(l.ApplyDurably({2000,PetAction::StartStory},&fx)==CommitResult::Committed);
    assert(l.Snapshot().mode==PetMode::Story);
    assert(l.PendingCount()==Bridge::kCapacity);

    Bridge::Record front{};
    for (size_t i=0;i<Bridge::kCapacity;++i) {
        assert(l.Peek(&front) && front.sequence==i+1);
        assert(Confirm(l,front,i+1)==Ledger::AckResult::Confirmed);
    }
    assert(!l.Peek(&front));
    assert(l.NextSequence()==Bridge::kCapacity+1);
    assert(l.ApplyDurably({2001,PetAction::NextPage},&fx)==CommitResult::Committed);
    assert(l.Snapshot().active_page==2);
    assert(l.PendingCount()==0);
    assert(l.ApplyDurably({3000,PetAction::Pickup},&fx)==CommitResult::Committed);
    assert(l.Peek(&front) && front.sequence==Bridge::kCapacity+1);
}

void TestStrictMigrationAndCorruption() {
    FakeNvs disk;
    Ledger empty(disk);
    assert(empty.Load()==Ledger::LoadResult::Empty);
    PetController legacy;
    PetEffects effects{};
    assert(legacy.Apply({123,PetAction::Feed},&effects)==PetResult::Applied);
    const PetCheckpoint old=legacy.ExportCheckpoint();
    disk.write_error=true;
    assert(!empty.ImportLegacyForFreshServer(old,5));
    assert(empty.Generation()==0);
    disk.write_error=false;
    assert(empty.ImportLegacyForFreshServer(old,5));
    assert(empty.Generation()==6);
    assert(!empty.InitializeFresh());
    assert(!empty.ImportLegacyForFreshServer(old,5));

    Ledger restored(disk);
    assert(restored.Load()==Ledger::LoadResult::Ready);
    assert(restored.Snapshot().interaction_count==1);
    assert(restored.NextSequence()==1);
    assert(restored.ApplyDurably({123,PetAction::Feed},&effects)==CommitResult::Duplicate);
    assert(restored.Snapshot().interaction_count==1);

    // Latest of two A/B blobs has CRC damage; use still-valid previous blob.
    assert(restored.ApplyDurably({124,PetAction::Pickup},&effects)==CommitResult::Committed);
    assert(restored.PendingCount()==1);
    disk.slots[0][36] ^= 0x1; // latest generation in the opposite slot
    Ledger fallback(disk);
    assert(fallback.Load()==Ledger::LoadResult::Ready);
    assert(fallback.Snapshot().interaction_count==1);
    assert(fallback.PendingCount()==0);
    disk.read_error=true;
    Ledger io_failure(disk);
    assert(io_failure.Load()==Ledger::LoadResult::StorageError);
    disk.read_error=false;

    FakeNvs corrupt;
    corrupt.present[0]=true;
    corrupt.slots[0].fill(0);
    Ledger refuse(corrupt);
    assert(refuse.Load()==Ledger::LoadResult::Corrupt);
    assert(!refuse.InitializeFresh()); // never erase corruption automatically
}

void TestChildQuietAndTransientRebootNormalization() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load()==Ledger::LoadResult::Empty);
    assert(l.InitializeFresh());
    PetEffects fx{};
    assert(l.ApplyDurably({123,PetAction::VoiceBegin},&fx)==CommitResult::Committed);
    assert(l.Snapshot().mode==PetMode::Listening);
    Ledger rebooted(disk);
    assert(rebooted.Load()==Ledger::LoadResult::Ready);
    assert(rebooted.Snapshot().mode==PetMode::Idle);
    assert(rebooted.PendingCount()==1); // VOICE_STARTED remains durable
    assert(rebooted.ApplyDurably({124,PetAction::QuietBegin},&fx)==CommitResult::Committed);
    assert(rebooted.Snapshot().quiet);
    assert(rebooted.ApplyDurably({125,PetAction::Feed},&fx)==CommitResult::Rejected);
    assert(rebooted.PendingCount()==2);
}

int main() {
    static_assert(Ledger::kBytes==1480, "Atomic journal format is fixed-size");
    TestAtomicFeedCommitCrashAndAck();
    TestCapacityAndNoLostUpdates();
    TestStrictMigrationAndCorruption();
    TestChildQuietAndTransientRebootNormalization();
    std::cout << "PASS: atomic single-blob pet+outbox, power-cut replay, ACK, full and migration guards\n";
}
