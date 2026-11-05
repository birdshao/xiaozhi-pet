#include "kid_app/pet_ledger_migration.h"

#include <memory>
#include <new>

namespace kid_app {
namespace {
using Migration = PetLedgerMigration;
using LedgerBlob = PetAtomicLedger::Store::Blob;
using MarkerBlob = Migration::MarkerStore::Blob;
constexpr uint16_t kMarkerVersion = 1;

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0);
    }
    return ~crc;
}
void Put64(MarkerBlob& b, size_t p, uint64_t n) {
    for (size_t i = 0; i < 8; ++i) b[p + i] = static_cast<uint8_t>(n >> (8 * i));
}
uint64_t Get64(const MarkerBlob& b, size_t p) {
    uint64_t n = 0;
    for (size_t i = 0; i < 8; ++i) n |= uint64_t(b[p + i]) << (8 * i);
    return n;
}
uint32_t Get32(const MarkerBlob& b, size_t p) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) n |= uint32_t(b[p + i]) << (8 * i);
    return n;
}

// Captures the one blob a fresh ledger writes (slot 0), never touching NVS.
class RamLedgerStore final : public PetAtomicLedger::Store {
public:
    BlobReadStatus Read(unsigned, Blob*, size_t*) override { return BlobReadStatus::NotFound; }
    bool Write(unsigned slot, const Blob& value) override {
        if (slot != 0 || written) return false;
        blob = value;
        written = true;
        return true;
    }
    Blob blob{};
    bool written = false;
};

// Large scratch (two blobs, three ledgers, the legacy controller) lives on
// the heap: Run() is called on a FreeRTOS task with a bounded stack.
struct Work {
    explicit Work(PetAtomicLedger::Store& store) : probe(store), check(store), expected(ram) {}
    PetAtomicLedger probe;
    PetAtomicLedger check;
    RamLedgerStore ram;
    PetAtomicLedger expected;
    PetController legacy;
    LedgerBlob read{};
};

bool SlotEmpty(PetAtomicLedger::Store& store, unsigned slot, LedgerBlob* scratch) {
    size_t length = 0;
    return store.Read(slot, scratch, &length) == BlobReadStatus::NotFound;
}

bool SlotHolds(PetAtomicLedger::Store& store, unsigned slot, const LedgerBlob& want,
               LedgerBlob* scratch) {
    size_t length = 0;
    return store.Read(slot, scratch, &length) == BlobReadStatus::Ok &&
           length == PetAtomicLedger::kBytes && *scratch == want;
}

Migration::Report Fail(Migration::Report report, Migration::Failure failure) {
    report.outcome = Migration::Outcome::Failed;
    report.failure = failure;
    return report;
}
}  // namespace

bool PetLedgerMigration::EncodeMarker(const Report& report, MarkerStore::Blob* out) {
    if (!out || report.ledger_generation == 0) return false;
    MarkerBlob b{};
    b[0] = 'K'; b[1] = 'P'; b[2] = 'M'; b[3] = 'G';
    b[4] = static_cast<uint8_t>(kMarkerVersion);
    b[5] = static_cast<uint8_t>(kMarkerVersion >> 8);
    b[6] = report.flags;
    Put64(b, 8, report.legacy_generation);
    Put64(b, 16, report.ledger_generation);
    const uint32_t crc = Crc32(b.data(), MarkerStore::kBytes - 4);
    for (size_t i = 0; i < 4; ++i) b[28 + i] = static_cast<uint8_t>(crc >> (8 * i));
    *out = b;
    return true;
}

bool PetLedgerMigration::DecodeMarker(const MarkerStore::Blob& b, size_t length, Report* out) {
    if (!out || length != MarkerStore::kBytes || b[0] != 'K' || b[1] != 'P' || b[2] != 'M' ||
        b[3] != 'G' || (b[4] | (b[5] << 8)) != kMarkerVersion || b[7] != 0 ||
        Get32(b, 24) != 0 || Get32(b, 28) != Crc32(b.data(), MarkerStore::kBytes - 4) ||
        Get64(b, 16) == 0)
        return false;
    out->flags = b[6];
    out->legacy_generation = Get64(b, 8);
    out->ledger_generation = Get64(b, 16);
    return true;
}

PetLedgerMigration::Report PetLedgerMigration::Run(PetAtomicLedger::Store& ledger,
                                                   PetBlobStore& legacy_pet,
                                                   ParentSource& legacy_parent,
                                                   bool legacy_bridge_present,
                                                   MarkerStore& marker,
                                                   LegacyRelease* release) {
    Report report{};
    std::unique_ptr<Work> work(new (std::nothrow) Work(ledger));
    if (!work) return Fail(report, Failure::LedgerUnreadable);

    // 1. Marker and current ledger, read only.
    MarkerBlob marker_blob{};
    size_t marker_length = 0;
    const BlobReadStatus marker_read = marker.Read(&marker_blob, &marker_length);
    if (marker_read == BlobReadStatus::Error) return Fail(report, Failure::MarkerUnreadable);
    const bool has_marker = marker_read == BlobReadStatus::Ok;
    if (has_marker && !DecodeMarker(marker_blob, marker_length, &report))
        return Fail(report, Failure::MarkerCorrupt);

    const auto loaded = work->probe.Load();
    if (loaded == PetAtomicLedger::LoadResult::StorageError)
        return Fail(report, Failure::LedgerUnreadable);
    if (has_marker) {
        if (loaded == PetAtomicLedger::LoadResult::Ready) {
            report.outcome = Outcome::AlreadyMigrated;
            // Verified earlier and still loadable: the old copy can go.
            if (release) report.legacy_released = release->Release();
            return report;
        }
        // The ledger was verified once; never rebuild it from older data.
        return Fail(report, loaded == PetAtomicLedger::LoadResult::Empty
                                ? Failure::MarkerWithoutLedger
                                : Failure::LedgerCorrupt);
    }

    // 2. Legacy sources (only read; they stay as the rollback copy).
    PetPersistence persistence(legacy_pet);
    switch (persistence.Load(&work->legacy)) {
        case PetLoadResult::Restored:
            report.flags |= kLegacyPet;
            report.legacy_generation = persistence.Generation();
            break;
        case PetLoadResult::NotFound:
            break;
        case PetLoadResult::Corrupt:  // the legacy service refused it as well
            return Fail(report, Failure::LegacyCorrupt);
        case PetLoadResult::StorageError:
            return Fail(report, Failure::LegacyUnreadable);
    }
    ParentSettings parent{};
    switch (legacy_parent.Load(&parent)) {
        case ParentSource::Result::Loaded:
            report.flags |= kLegacyParent;
            break;
        case ParentSource::Result::NotFound:
            parent = ParentSettings{};
            break;
        case ParentSource::Result::Corrupt:  // the board used the defaults
            parent = ParentSettings{};
            report.flags |= kLegacyParentCorrupt;
            break;
        case ParentSource::Result::StorageError:
            return Fail(report, Failure::ParentUnreadable);
    }
    if (legacy_bridge_present) report.flags |= kLegacyBridge;

    // 3. The exact first ledger blob, built in RAM (deterministic encoding).
    if (work->expected.Load() != PetAtomicLedger::LoadResult::Empty ||
        !work->expected.ImportLegacyLocal(work->legacy.ExportCheckpoint(),
                                          report.legacy_generation, parent) ||
        !work->ram.written)
        return Fail(report, Failure::LegacyCorrupt);
    report.ledger_generation = work->expected.Generation();
    const LedgerBlob& want = work->ram.blob;

    // 4. Write it, unless an interrupted earlier boot already did.
    if (loaded == PetAtomicLedger::LoadResult::Ready) {
        // No marker yet: only our own first write can be there, untouched.
        if (!SlotHolds(ledger, 0, want, &work->read) || !SlotEmpty(ledger, 1, &work->read))
            return Fail(report, Failure::LedgerMismatch);
        report.outcome = Outcome::Resumed;
    } else {
        if (loaded == PetAtomicLedger::LoadResult::Corrupt) {
            // A torn first write is only ever in slot 0; anything in slot 1
            // was not written by this migration: leave it for a person.
            if (!SlotEmpty(ledger, 1, &work->read)) return Fail(report, Failure::LedgerCorrupt);
            report.flags |= kRewroteUnverified;
        }
        if (!ledger.Write(0, want)) return Fail(report, Failure::LedgerWriteFailed);
        report.outcome = Outcome::Migrated;
    }

    // 5. Verify: byte-exact read-back, then a full independent load.
    if (!SlotHolds(ledger, 0, want, &work->read) ||
        work->check.Load() != PetAtomicLedger::LoadResult::Ready ||
        work->check.Generation() != report.ledger_generation || work->check.Parent() != parent ||
        work->check.PendingCount() != 0)
        return Fail(report, Failure::VerifyFailed);

    // 6. Only now: the marker. A cut before it repeats steps 1-5 (Resumed).
    if (!EncodeMarker(report, &marker_blob) || !marker.Write(marker_blob))
        return Fail(report, Failure::MarkerWriteFailed);
    // 7. Marked: the legacy keys are no longer needed to verify anything.
    if (release) report.legacy_released = release->Release();
    return report;
}

const char* PetLedgerMigration::OutcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::AlreadyMigrated: return "already";
        case Outcome::Migrated: return "migrated";
        case Outcome::Resumed: return "resumed";
        case Outcome::Failed: return "failed";
    }
    return "unknown";
}

const char* PetLedgerMigration::FailureName(Failure failure) {
    switch (failure) {
        case Failure::None: return "none";
        case Failure::MarkerUnreadable: return "marker_unreadable";
        case Failure::MarkerCorrupt: return "marker_corrupt";
        case Failure::MarkerWithoutLedger: return "marker_without_ledger";
        case Failure::LedgerUnreadable: return "ledger_unreadable";
        case Failure::LedgerCorrupt: return "ledger_corrupt";
        case Failure::LedgerMismatch: return "ledger_mismatch";
        case Failure::LegacyUnreadable: return "legacy_unreadable";
        case Failure::LegacyCorrupt: return "legacy_corrupt";
        case Failure::ParentUnreadable: return "parent_unreadable";
        case Failure::LedgerWriteFailed: return "ledger_write_failed";
        case Failure::VerifyFailed: return "verify_failed";
        case Failure::MarkerWriteFailed: return "marker_write_failed";
    }
    return "unknown";
}

}  // namespace kid_app
