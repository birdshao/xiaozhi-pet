#include "kid_app/pet_persistence.h"

#include <limits>

namespace kid_app {
namespace {
using Blob = PetCheckpointCodec::Blob;

void Put16(Blob& b, size_t at, uint16_t value) {
    for (size_t i = 0; i < 2; ++i)
        b[at + i] = static_cast<uint8_t>(value >> (8 * i));
}
void Put32(Blob& b, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
        b[at + i] = static_cast<uint8_t>(value >> (8 * i));
}
void Put64(Blob& b, size_t at, uint64_t value) {
    for (size_t i = 0; i < 8; ++i)
        b[at + i] = static_cast<uint8_t>(value >> (8 * i));
}
uint16_t Get16(const Blob& b, size_t at) {
    return uint16_t(b[at]) | (uint16_t(b[at + 1]) << 8);
}
uint32_t Get32(const Blob& b, size_t at) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value |= uint32_t(b[at + i]) << (8 * i);
    return value;
}
uint64_t Get64(const Blob& b, size_t at) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value |= uint64_t(b[at + i]) << (8 * i);
    return value;
}

uint32_t Crc32(const Blob& b) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < b.size() - 4; ++i) {
        crc ^= b[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~crc;
}
}  // namespace

bool PetCheckpointCodec::Encode(const PetCheckpoint& checkpoint,
                                uint64_t generation, Blob* out) {
    if (!out || generation == 0) return false;
    // Reuse the controller's strict invariant checks rather than accepting
    // invalid enums or a nonunique event replay window.
    PetController validator;
    if (!validator.RestoreCheckpoint(checkpoint)) return false;

    Blob blob{};
    blob[0] = 'K'; blob[1] = 'P'; blob[2] = 'E'; blob[3] = 'T';
    Put16(blob, 4, 1);
    Put16(blob, 6, static_cast<uint16_t>(kBytes));
    Put64(blob, 8, generation);
    const auto& s = checkpoint.snapshot;
    Put32(blob, 16, s.schema_version);
    blob[20] = static_cast<uint8_t>(s.mode);
    blob[21] = static_cast<uint8_t>(s.mood);
    blob[22] = s.quiet ? 1 : 0;
    Put32(blob, 24, s.interaction_count);
    Put16(blob, 28, s.active_page);
    Put16(blob, 30, checkpoint.seen_count);
    Put16(blob, 32, checkpoint.next_slot);
    for (size_t i = 0; i < PetCheckpoint::kSeenCapacity; ++i) {
        Put64(blob, 36 + 8 * i, checkpoint.seen[i]);
    }
    Put32(blob, kBytes - 4, Crc32(blob));
    *out = blob;
    return true;
}

bool PetCheckpointCodec::Decode(const Blob& blob, PetCheckpoint* out,
                                uint64_t* generation) {
    if (!out || !generation || blob[0] != 'K' || blob[1] != 'P' ||
        blob[2] != 'E' || blob[3] != 'T' ||
        Get16(blob, 4) != 1 || Get16(blob, 6) != kBytes ||
        blob[23] != 0 || blob[34] != 0 || blob[35] != 0 ||
        blob[22] > 1 || Get32(blob, kBytes - 4) != Crc32(blob)) {
        return false;
    }
    const auto loaded_gen = Get64(blob, 8);
    if (loaded_gen == 0) return false;
    PetCheckpoint loaded{};
    loaded.snapshot.schema_version = Get32(blob, 16);
    loaded.snapshot.mode = static_cast<PetMode>(blob[20]);
    loaded.snapshot.mood = static_cast<PetMood>(blob[21]);
    loaded.snapshot.quiet = blob[22] != 0;
    loaded.snapshot.interaction_count = Get32(blob, 24);
    loaded.snapshot.active_page = Get16(blob, 28);
    loaded.seen_count = Get16(blob, 30);
    loaded.next_slot = Get16(blob, 32);
    for (size_t i = 0; i < PetCheckpoint::kSeenCapacity; ++i) {
        loaded.seen[i] = Get64(blob, 36 + i * 8);
    }
    PetController validator;
    if (!validator.RestoreCheckpoint(loaded)) return false;
    *out = loaded;  // No caller-visible mutation on validation failure.
    *generation = loaded_gen;
    return true;
}

PetLoadResult PetPersistence::Load(PetController* pet) {
    if (!pet) return PetLoadResult::StorageError;
    PetCheckpoint best{};
    uint64_t best_gen = 0;
    int best_slot = -1;
    bool corrupt = false, failed = false;
    for (unsigned slot = 0; slot != 2; ++slot) {
        PetCheckpointCodec::Blob blob{};
        const auto status = storage_.Read(slot, &blob);
        if (status == BlobReadStatus::Error) { failed = true; continue; }
        if (status == BlobReadStatus::NotFound) continue;
        PetCheckpoint candidate{};
        uint64_t gen = 0;
        if (!PetCheckpointCodec::Decode(blob, &candidate, &gen)) {
            corrupt = true;
            continue;
        }
        if (gen > best_gen) {
            best = candidate;
            best_gen = gen;
            best_slot = static_cast<int>(slot);
        }
    }
    // A failed read could conceal a newer valid slot. Fail closed rather
    // than load an older checkpoint then overwrite the inaccessible one.
    if (failed) return PetLoadResult::StorageError;
    if (best_slot < 0) {
        return corrupt ? PetLoadResult::Corrupt : PetLoadResult::NotFound;
    }
    // An unclean reboot cannot resume an old microphone / TTS session.
    if (best.snapshot.mode == PetMode::Listening ||
        best.snapshot.mode == PetMode::Speaking) {
        best.snapshot.mode = PetMode::Idle;
        best.snapshot.mood = PetMood::Neutral;
    }
    if (!pet->RestoreCheckpoint(best)) return PetLoadResult::Corrupt;
    generation_ = best_gen;
    active_slot_ = best_slot;
    has_saved_time_ = false;
    return PetLoadResult::Restored;
}

bool PetPersistence::Save(const PetController& pet) {
    if (generation_ == std::numeric_limits<uint64_t>::max()) return false;
    const uint64_t next_gen = generation_ + 1;
    const unsigned next_slot = active_slot_ == 0 ? 1u : 0u;
    PetCheckpointCodec::Blob blob{};
    if (!PetCheckpointCodec::Encode(pet.ExportCheckpoint(), next_gen, &blob) ||
        !storage_.Write(next_slot, blob)) return false;

    // Only advance the live pointer after the new blob commits.
    generation_ = next_gen;
    active_slot_ = static_cast<int>(next_slot);
    return true;
}

bool PetPersistence::SaveIfDue(const PetController& pet, uint64_t now_ms,
                               uint64_t minimum_interval_ms, bool force) {
    if (!force && has_saved_time_ && now_ms >= last_saved_ms_ &&
        (now_ms - last_saved_ms_) < minimum_interval_ms) {
        return false;
    }
    if (!Save(pet)) return false;
    last_saved_ms_ = now_ms;
    has_saved_time_ = true;
    return true;
}

}  // namespace kid_app
