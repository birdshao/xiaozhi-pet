// Board switch to the atomic ledger: one-time migration from the legacy
// kid_pet checkpoint and the V3-06 kid_parent/settings key, with simulated
// NVS (power loss at every step) and the local-only (no outbox) mode.
#include "kid_app/pet_ledger_migration.h"
#include "kid_app/pet_local_event_id.h"

#include <cassert>
#include <iostream>

using namespace kid_app;
using Ledger = PetAtomicLedger;
using Migration = PetLedgerMigration;

namespace {

class FakeLedgerNvs : public Ledger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out, size_t* length) override {
        if (read_error || slot > 1 || !out || !length) return BlobReadStatus::Error;
        if (!present[slot]) return BlobReadStatus::NotFound;
        *out = slots[slot];
        if (flip_on_read) (*out)[40] ^= 1;
        *length = lengths[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const Blob& data) override {
        ++writes;
        if (write_error || slot > 1) return false;
        slots[slot] = data;
        lengths[slot] = data.size();
        present[slot] = true;
        if (tear_writes) slots[slot][100] ^= 0x5A;  // committed but torn
        return true;
    }
    Blob slots[2]{};
    size_t lengths[2]{};
    bool present[2]{};
    bool read_error = false, write_error = false, tear_writes = false, flip_on_read = false;
    int writes = 0;
};

class FakeLegacyPet : public PetBlobStore {
public:
    BlobReadStatus Read(unsigned slot, PetCheckpointCodec::Blob* out) override {
        if (read_error || slot > 1 || !out) return BlobReadStatus::Error;
        if (!present[slot]) return BlobReadStatus::NotFound;
        *out = slots[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const PetCheckpointCodec::Blob& value) override {
        ++writes;
        if (slot > 1) return false;
        slots[slot] = value;
        present[slot] = true;
        return true;
    }
    PetCheckpointCodec::Blob slots[2]{};
    bool present[2]{};
    bool read_error = false;
    int writes = 0;
};

class FakeParent : public Migration::ParentSource {
public:
    Result Load(ParentSettings* out) override {
        ++reads;
        if (result == Result::Loaded) *out = settings;
        return result;
    }
    Result result = Result::NotFound;
    ParentSettings settings{};
    int reads = 0;
};

class FakeMarker : public Migration::MarkerStore {
public:
    BlobReadStatus Read(Blob* out, size_t* length) override {
        if (read_error) return BlobReadStatus::Error;
        if (!present) return BlobReadStatus::NotFound;
        *out = blob;
        *length = length_;
        return BlobReadStatus::Ok;
    }
    bool Write(const Blob& value) override {
        ++writes;
        if (write_error) return false;
        blob = value;
        length_ = value.size();
        present = true;
        return true;
    }
    Blob blob{};
    size_t length_ = 0;
    bool present = false, read_error = false, write_error = false;
    int writes = 0;
};

struct Device {
    FakeLedgerNvs ledger;
    FakeLegacyPet pet;
    FakeParent parent;
    FakeMarker marker;
    bool bridge = false;
    Migration::Report Run() { return Migration::Run(ledger, pet, parent, bridge, marker); }
};

// A real legacy history: feeds and a greeting saved by PetPersistence, as
// the old FreertosPetService did (generation 3, three replay ids).
uint64_t SeedLegacyPet(FakeLegacyPet& store, PetMode final_mode = PetMode::Idle) {
    PetController pet;
    PetPersistence persistence(store);
    assert(persistence.Load(&pet) == PetLoadResult::NotFound);
    const PetAction actions[] = {PetAction::Feed, PetAction::Pickup, PetAction::Feed};
    for (PetAction action : actions) {
        PetEffects effects{};
        assert(pet.Apply({NextLocalEventId(persistence.Generation()), action}, &effects) ==
               PetResult::Applied);
        assert(persistence.Save(pet));
    }
    if (final_mode == PetMode::Listening) {
        PetEffects effects{};
        assert(pet.Apply({NextLocalEventId(persistence.Generation()), PetAction::VoiceBegin},
                         &effects) == PetResult::Applied);
        assert(pet.Snapshot().mode == PetMode::Listening);
        assert(persistence.Save(pet));
    }
    store.writes = 0;
    return persistence.Generation();
}

ParentSettings CustomParent() {
    ParentSettings s{};
    s.volume_cap_pct = 60;
    s.quiet_volume_pct = 0;
    s.motion_enabled = false;
    s.schedule.wake_min = 7 * 60 + 30;
    assert(ValidParentSettings(s));
    return s;
}

void TestFreshDeviceMigratesOnceThenIsIdempotent() {
    Device d;
    const auto first = d.Run();
    assert(first.outcome == Migration::Outcome::Migrated);
    assert(first.failure == Migration::Failure::None);
    assert(first.flags == 0 && first.legacy_generation == 0 && first.ledger_generation == 1);
    assert(d.ledger.writes == 1 && d.marker.writes == 1 && d.pet.writes == 0);
    assert(d.ledger.present[0] && !d.ledger.present[1]);

    Ledger ledger(d.ledger);
    assert(ledger.Load() == Ledger::LoadResult::Ready);
    assert(ledger.Generation() == 1 && ledger.Parent() == ParentSettings{});
    assert(ledger.Snapshot().interaction_count == 0 && ledger.PendingCount() == 0);

    // Every later boot: nothing is read from legacy, nothing is written.
    for (int boot = 0; boot < 3; ++boot) {
        const auto again = d.Run();
        assert(again.outcome == Migration::Outcome::AlreadyMigrated);
        assert(again.ledger_generation == 1);
    }
    assert(d.ledger.writes == 1 && d.marker.writes == 1 && d.parent.reads == 1);
}

void TestLegacyPetAndParentCarriedOver() {
    Device d;
    const uint64_t legacy_gen = SeedLegacyPet(d.pet);
    d.parent.result = Migration::ParentSource::Result::Loaded;
    d.parent.settings = CustomParent();
    const auto before0 = d.pet.slots[0], before1 = d.pet.slots[1];

    const auto report = d.Run();
    assert(report.outcome == Migration::Outcome::Migrated);
    assert(report.flags == (Migration::kLegacyPet | Migration::kLegacyParent));
    assert(report.legacy_generation == legacy_gen && legacy_gen == 3);
    assert(report.ledger_generation == legacy_gen + 1);

    Ledger ledger(d.ledger);
    assert(ledger.Load() == Ledger::LoadResult::Ready);
    assert(ledger.Snapshot().interaction_count == 2);
    assert(ledger.Snapshot().mood == PetMood::Happy);
    assert(ledger.Parent() == CustomParent());
    assert(ledger.Memory() == MemoryStore{});
    assert(ledger.Generation() == legacy_gen + 1);

    // Old replay ids stay known (a replayed old event never double-counts)
    // and the next local id is new.
    PetEffects effects{};
    assert(ledger.ApplyDurably({NextLocalEventId(0), PetAction::Feed}, &effects) ==
           CommitResult::Duplicate);
    assert(ledger.ApplyDurably({NextLocalEventId(legacy_gen - 1), PetAction::Feed}, &effects) ==
           CommitResult::Duplicate);
    assert(ledger.ApplyDurably({NextLocalEventId(ledger.Generation()), PetAction::Feed},
                               &effects) == CommitResult::Committed);
    assert(ledger.Snapshot().interaction_count == 3);

    // Legacy keys are only read: byte-identical, never written.
    assert(d.pet.writes == 0 && d.pet.slots[0] == before0 && d.pet.slots[1] == before1);
}

void TestPowerCutBeforeMarkerResumes() {
    Device d;
    SeedLegacyPet(d.pet);
    d.parent.result = Migration::ParentSource::Result::Loaded;
    d.parent.settings = CustomParent();
    d.marker.write_error = true;  // power lost right after the ledger commit
    const auto cut = d.Run();
    assert(cut.outcome == Migration::Outcome::Failed);
    assert(cut.failure == Migration::Failure::MarkerWriteFailed);
    assert(d.ledger.present[0] && !d.marker.present);
    const auto written = d.ledger.slots[0];

    d.marker.write_error = false;
    const auto resumed = d.Run();
    assert(resumed.outcome == Migration::Outcome::Resumed);
    assert(resumed.flags == (Migration::kLegacyPet | Migration::kLegacyParent));
    assert(d.ledger.writes == 1);  // the verified blob is not written again
    assert(d.ledger.slots[0] == written && d.marker.present);
    assert(d.Run().outcome == Migration::Outcome::AlreadyMigrated);
    assert(d.pet.writes == 0);
}

void TestLedgerWriteFailureLeavesNothing() {
    Device d;
    SeedLegacyPet(d.pet);
    d.ledger.write_error = true;
    const auto failed = d.Run();
    assert(failed.outcome == Migration::Outcome::Failed);
    assert(failed.failure == Migration::Failure::LedgerWriteFailed);
    assert(!d.ledger.present[0] && !d.ledger.present[1] && !d.marker.present);
    d.ledger.write_error = false;
    assert(d.Run().outcome == Migration::Outcome::Migrated);
    assert(d.marker.present && d.pet.writes == 0);
}

void TestTornFirstWriteIsRewrittenFromLegacy() {
    Device d;
    SeedLegacyPet(d.pet);
    d.ledger.tear_writes = true;
    const auto torn = d.Run();
    assert(torn.outcome == Migration::Outcome::Failed);
    assert(torn.failure == Migration::Failure::VerifyFailed);
    assert(!d.marker.present);
    {
        Ledger probe(d.ledger);
        assert(probe.Load() == Ledger::LoadResult::Corrupt);
    }
    d.ledger.tear_writes = false;
    const auto fixed = d.Run();
    assert(fixed.outcome == Migration::Outcome::Migrated);
    assert(fixed.flags & Migration::kRewroteUnverified);
    Ledger ledger(d.ledger);
    assert(ledger.Load() == Ledger::LoadResult::Ready);
    assert(ledger.Snapshot().interaction_count == 2);
}

void TestReadBackMismatchNeverMarks() {
    Device d;
    SeedLegacyPet(d.pet);
    d.ledger.flip_on_read = true;
    const auto report = d.Run();
    assert(report.outcome == Migration::Outcome::Failed);
    assert(report.failure == Migration::Failure::VerifyFailed);
    assert(!d.marker.present && d.marker.writes == 0);
}

void TestCorruptUnknownLedgerIsNotTouched() {
    Device d;
    SeedLegacyPet(d.pet);
    d.ledger.present[1] = true;  // something else in slot 1: not ours
    d.ledger.lengths[1] = Ledger::kBytes;
    d.ledger.slots[1].fill(0x42);
    const auto report = d.Run();
    assert(report.outcome == Migration::Outcome::Failed);
    assert(report.failure == Migration::Failure::LedgerCorrupt);
    assert(d.ledger.writes == 0 && d.marker.writes == 0);
}

void TestForeignLedgerWithoutMarkerFailsClosed() {
    Device d;
    SeedLegacyPet(d.pet);
    {
        // A ledger with its own history (e.g. FreshNoLegacy firmware).
        Ledger other(d.ledger);
        assert(other.Load() == Ledger::LoadResult::Empty);
        assert(other.InitializeFresh());
        PetEffects effects{};
        assert(other.ApplyDurably({NextLocalEventId(other.Generation()), PetAction::Feed},
                                  &effects) == CommitResult::Committed);
    }
    const int writes = d.ledger.writes;
    const auto report = d.Run();
    assert(report.outcome == Migration::Outcome::Failed);
    assert(report.failure == Migration::Failure::LedgerMismatch);
    assert(d.ledger.writes == writes && d.marker.writes == 0);
}

void TestMarkerProblemsFailClosed() {
    {
        Device d;
        assert(d.Run().outcome == Migration::Outcome::Migrated);
        d.ledger.present[0] = false;  // ledger gone, marker still there
        const auto report = d.Run();
        assert(report.failure == Migration::Failure::MarkerWithoutLedger);
        assert(d.ledger.writes == 1);  // never rebuilt from older data
    }
    {
        Device d;
        assert(d.Run().outcome == Migration::Outcome::Migrated);
        d.marker.blob[9] ^= 1;
        assert(d.Run().failure == Migration::Failure::MarkerCorrupt);
        d.marker.blob[9] ^= 1;
        d.marker.length_ = 31;
        assert(d.Run().failure == Migration::Failure::MarkerCorrupt);
    }
    {
        Device d;
        d.marker.read_error = true;
        assert(d.Run().failure == Migration::Failure::MarkerUnreadable);
        assert(d.ledger.writes == 0);
    }
    {
        Device d;
        d.ledger.read_error = true;
        assert(d.Run().failure == Migration::Failure::LedgerUnreadable);
        assert(d.marker.writes == 0);
    }
}

void TestLegacyAndParentErrors() {
    {
        Device d;
        SeedLegacyPet(d.pet);
        d.pet.slots[0][20] ^= 1;
        d.pet.slots[1][20] ^= 1;
        assert(d.Run().failure == Migration::Failure::LegacyCorrupt);
        assert(d.ledger.writes == 0 && d.pet.writes == 0);
    }
    {
        Device d;
        d.pet.read_error = true;
        assert(d.Run().failure == Migration::Failure::LegacyUnreadable);
        assert(d.ledger.writes == 0);
    }
    {
        Device d;
        d.parent.result = Migration::ParentSource::Result::StorageError;
        assert(d.Run().failure == Migration::Failure::ParentUnreadable);
        assert(d.ledger.writes == 0);
        d.parent.result = Migration::ParentSource::Result::NotFound;
        assert(d.Run().outcome == Migration::Outcome::Migrated);
    }
    {
        // Corrupt board key: the board ran on the defaults, so does the ledger.
        Device d;
        d.parent.result = Migration::ParentSource::Result::Corrupt;
        d.bridge = true;
        const auto report = d.Run();
        assert(report.outcome == Migration::Outcome::Migrated);
        assert(report.flags == (Migration::kLegacyParentCorrupt | Migration::kLegacyBridge));
        Ledger ledger(d.ledger);
        assert(ledger.Load() == Ledger::LoadResult::Ready);
        assert(ledger.Parent() == ParentSettings{});
        // The flags survive in the marker for later boots.
        const auto later = d.Run();
        assert(later.outcome == Migration::Outcome::AlreadyMigrated);
        assert(later.flags == report.flags);
    }
}

void TestLaterCommitsAreNeverRolledBack() {
    Device d;
    SeedLegacyPet(d.pet);
    assert(d.Run().outcome == Migration::Outcome::Migrated);
    uint64_t gen = 0;
    {
        Ledger ledger(d.ledger);
        assert(ledger.Load() == Ledger::LoadResult::Ready);
        PetEffects effects{};
        for (int i = 0; i < 5; ++i)
            assert(ledger.ApplyDurably({NextLocalEventId(ledger.Generation()), PetAction::Feed},
                                       &effects) == CommitResult::Committed);
        assert(ledger.SetParentSettings(NextLocalEventId(ledger.Generation()), CustomParent()) ==
               CommitResult::Committed);
        gen = ledger.Generation();
    }
    const int writes = d.ledger.writes;
    assert(d.Run().outcome == Migration::Outcome::AlreadyMigrated);
    assert(d.ledger.writes == writes);
    Ledger after(d.ledger);
    assert(after.Load() == Ledger::LoadResult::Ready);
    assert(after.Generation() == gen && after.Snapshot().interaction_count == 7);
    assert(after.Parent() == CustomParent());
}

void TestInterruptedVoiceIsNormalized() {
    Device d;
    SeedLegacyPet(d.pet, PetMode::Listening);
    assert(d.Run().outcome == Migration::Outcome::Migrated);
    Ledger ledger(d.ledger);
    assert(ledger.Load() == Ledger::LoadResult::Ready);
    assert(ledger.Snapshot().mode == PetMode::Idle);
}

// Whatever step a power cut hits, the device ends with the same ledger as
// an uninterrupted migration and the legacy keys untouched.
void TestEveryCutConvergesToTheSameLedger() {
    Device clean;
    SeedLegacyPet(clean.pet);
    clean.parent.result = Migration::ParentSource::Result::Loaded;
    clean.parent.settings = CustomParent();
    assert(clean.Run().outcome == Migration::Outcome::Migrated);

    for (int cut = 0; cut < 4; ++cut) {
        Device d;
        SeedLegacyPet(d.pet);
        d.parent.result = Migration::ParentSource::Result::Loaded;
        d.parent.settings = CustomParent();
        d.ledger.write_error = cut == 0;
        d.ledger.tear_writes = cut == 1;
        d.marker.write_error = cut == 2;
        d.ledger.flip_on_read = cut == 3;
        assert(d.Run().outcome == Migration::Outcome::Failed);
        d.ledger.write_error = d.ledger.tear_writes = d.marker.write_error = false;
        d.ledger.flip_on_read = false;
        const auto report = d.Run();
        assert(report.outcome == Migration::Outcome::Migrated ||
               report.outcome == Migration::Outcome::Resumed);
        assert(d.ledger.slots[0] == clean.ledger.slots[0]);
        assert(d.marker.blob == clean.marker.blob || (report.flags & Migration::kRewroteUnverified));
        assert(d.pet.writes == 0);
        assert(d.Run().outcome == Migration::Outcome::AlreadyMigrated);
    }
}

void TestMarkerCodec() {
    Migration::Report report{};
    report.flags = Migration::kLegacyPet | Migration::kLegacyBridge;
    report.legacy_generation = 41;
    report.ledger_generation = 42;
    Migration::MarkerStore::Blob blob{};
    assert(Migration::EncodeMarker(report, &blob));
    Migration::Report decoded{};
    assert(Migration::DecodeMarker(blob, blob.size(), &decoded));
    assert(decoded.flags == report.flags && decoded.legacy_generation == 41 &&
           decoded.ledger_generation == 42);
    for (size_t i = 0; i < blob.size(); ++i) {
        auto bad = blob;
        bad[i] ^= 0x10;
        assert(!Migration::DecodeMarker(bad, bad.size(), &decoded));
    }
    report.ledger_generation = 0;
    assert(!Migration::EncodeMarker(report, &blob));
}

void TestLocalOnlyNeverFillsTheOutbox() {
    FakeLedgerNvs disk;
    Ledger ledger(disk);
    assert(ledger.Load() == Ledger::LoadResult::Empty);
    assert(ledger.ImportLegacyLocal(PetController{}.ExportCheckpoint(), 0, ParentSettings{}));
    ledger.SetProjection(false);
    assert(!ledger.Projection());
    PetEffects effects{};
    // Far beyond the 16-slot outbox: every feed still commits.
    for (int i = 0; i < 40; ++i)
        assert(ledger.ApplyDurably({NextLocalEventId(ledger.Generation()), PetAction::Feed},
                                   &effects, 20000) == CommitResult::Committed);
    assert(ledger.PendingCount() == 0 && ledger.NextSequence() == 1);
    assert(ledger.Snapshot().interaction_count == 40);
    assert(ledger.Memory().Today().feeds == 40 && ledger.Memory().Today().day == 20000);
    PetDeviceBridge::Record head{};
    assert(!ledger.Peek(&head));

    // Projection on (the default) keeps the original outbox behaviour.
    FakeLedgerNvs disk2;
    Ledger projected(disk2);
    assert(projected.Load() == Ledger::LoadResult::Empty);
    assert(projected.InitializeFresh());
    assert(projected.Projection());
    assert(projected.ApplyDurably({NextLocalEventId(projected.Generation()), PetAction::Feed},
                                  &effects) == CommitResult::Committed);
    assert(projected.PendingCount() == 1);
}

void TestImportLegacyLocalPreconditions() {
    FakeLedgerNvs disk;
    Ledger ledger(disk);
    ParentSettings invalid{};
    invalid.volume_cap_pct = 55;
    assert(!ledger.ImportLegacyLocal(PetController{}.ExportCheckpoint(), 0, ParentSettings{}));
    assert(ledger.Load() == Ledger::LoadResult::Empty);
    assert(!ledger.ImportLegacyLocal(PetController{}.ExportCheckpoint(), 0, invalid));
    assert(disk.writes == 0);
    assert(ledger.ImportLegacyLocal(PetController{}.ExportCheckpoint(), 7, CustomParent()));
    assert(ledger.Generation() == 8 && ledger.Parent() == CustomParent());
    assert(!ledger.ImportLegacyLocal(PetController{}.ExportCheckpoint(), 7, CustomParent()));
}

}  // namespace

int main() {
    TestFreshDeviceMigratesOnceThenIsIdempotent();
    TestLegacyPetAndParentCarriedOver();
    TestPowerCutBeforeMarkerResumes();
    TestLedgerWriteFailureLeavesNothing();
    TestTornFirstWriteIsRewrittenFromLegacy();
    TestReadBackMismatchNeverMarks();
    TestCorruptUnknownLedgerIsNotTouched();
    TestForeignLedgerWithoutMarkerFailsClosed();
    TestMarkerProblemsFailClosed();
    TestLegacyAndParentErrors();
    TestLaterCommitsAreNeverRolledBack();
    TestInterruptedVoiceIsNormalized();
    TestEveryCutConvergesToTheSameLedger();
    TestMarkerCodec();
    TestLocalOnlyNeverFillsTheOutbox();
    TestImportLegacyLocalPreconditions();
    std::cout << "pet ledger migration tests passed\n";
    return 0;
}
