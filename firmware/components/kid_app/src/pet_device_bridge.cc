#include "kid_app/pet_device_bridge.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>

namespace kid_app {
namespace {
using Bridge = PetDeviceBridge;
using Blob = Bridge::Storage::Blob;
constexpr size_t kHeader = 28;
constexpr size_t kRecordBytes = 24;
constexpr uint64_t kMaxSeq = static_cast<uint64_t>(INT64_MAX);

void Put16(Blob& bytes, size_t offset, uint16_t n) {
    bytes[offset] = static_cast<uint8_t>(n);
    bytes[offset + 1] = static_cast<uint8_t>(n >> 8);
}
void Put32(Blob& bytes, size_t offset, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(n >> (8 * i));
}
void Put64(Blob& bytes, size_t offset, uint64_t n) {
    for (size_t i = 0; i < 8; ++i) bytes[offset + i] = static_cast<uint8_t>(n >> (8 * i));
}
uint16_t Get16(const Blob& b, size_t p) {
    return uint16_t(b[p]) | (uint16_t(b[p + 1]) << 8);
}
uint32_t Get32(const Blob& b, size_t p) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) n |= uint32_t(b[p + i]) << (8 * i);
    return n;
}
uint64_t Get64(const Blob& b, size_t p) {
    uint64_t n = 0;
    for (size_t i = 0; i < 8; ++i) n |= uint64_t(b[p + i]) << (8 * i);
    return n;
}
uint32_t Crc32(const Blob& b) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < b.size() - 4; ++i) {
        crc ^= b[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}
bool ValidId(const char* id) {
    if (!id) return false;
    const size_t size = std::strlen(id);
    if (size < 3 || size > 63) return false;
    for (size_t i = 0; i < size; ++i) {
        const char c = id[i];
        const bool acceptable = (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            (i != 0 && (c == '-' || c == '_'));
        if (!acceptable) return false;
    }
    return true;
}
}  // namespace

const char* PetDeviceBridge::ServerAction(PetAction action) {
    // Do not map events whose payload/context cannot be reconstructed
    // from the current PetEvent{uint64_t id, PetAction action}.
    switch (action) {
        case PetAction::Pickup: return "PET_PICKUP";
        case PetAction::Feed: return "PET_FEED";
        case PetAction::QuietBegin: return "QUIET_HOURS_START";
        case PetAction::QuietEnd: return "QUIET_HOURS_END";
        case PetAction::VoiceBegin: return "VOICE_STARTED";
        case PetAction::VoiceEnd: return "VOICE_ENDED";
        default: return nullptr;
    }
}

bool PetDeviceBridge::Encode(const State& s, uint64_t gen, Blob* out) {
    if (!out || gen == 0 || s.next_sequence == 0 ||
        s.next_sequence > kMaxSeq || s.count > kCapacity ||
        s.next_sequence <= s.count) return false;
    Blob b{};
    b[0] = 'K'; b[1] = 'B'; b[2] = 'R'; b[3] = 'G';
    Put16(b, 4, 1);
    Put16(b, 6, static_cast<uint16_t>(b.size()));
    Put64(b, 8, gen);
    Put64(b, 16, s.next_sequence);
    Put16(b, 24, s.count);
    for (size_t i = 0; i < s.count; ++i) {
        const Record& entry = s.pending[i];
        if (!entry.event_id || entry.sequence != s.next_sequence - s.count + i ||
            !ServerAction(entry.action)) return false;
        for (size_t j = 0; j < i; ++j)
            if (s.pending[j].event_id == entry.event_id) return false;
        const size_t p = kHeader + i * kRecordBytes;
        Put64(b, p, entry.event_id);
        Put64(b, p + 8, entry.sequence);
        b[p + 16] = static_cast<uint8_t>(entry.action);
    }
    Put32(b, b.size() - 4, Crc32(b));
    *out = b;
    return true;
}

bool PetDeviceBridge::Decode(const Blob& bytes, State* out, uint64_t* gen) {
    if (!out || !gen || bytes[0] != 'K' || bytes[1] != 'B' ||
        bytes[2] != 'R' || bytes[3] != 'G' ||
        Get16(bytes, 4) != 1 || Get16(bytes, 6) != bytes.size() ||
        Get32(bytes, bytes.size() - 4) != Crc32(bytes) ||
        bytes[26] != 0 || bytes[27] != 0) return false;
    State s{};
    const uint64_t generation = Get64(bytes, 8);
    s.next_sequence = Get64(bytes, 16);
    s.count = Get16(bytes, 24);
    if (generation == 0 || s.count > kCapacity ||
        s.next_sequence == 0 || s.next_sequence > kMaxSeq ||
        s.next_sequence <= s.count) return false;
    for (size_t i = 0; i < kCapacity; ++i) {
        const size_t p = kHeader + i * kRecordBytes;
        if (i < s.count) {
            s.pending[i].event_id = Get64(bytes, p);
            s.pending[i].sequence = Get64(bytes, p + 8);
            s.pending[i].action = static_cast<PetAction>(bytes[p + 16]);
            for (size_t j = 17; j < kRecordBytes; ++j)
                if (bytes[p + j] != 0) return false;
        } else {
            for (size_t j = 0; j < kRecordBytes; ++j)
                if (bytes[p + j] != 0) return false;
        }
    }
    Blob valid{};
    if (!Encode(s, generation, &valid) || valid != bytes) return false;
    *gen = generation;
    *out = s;
    return true;
}

PetDeviceBridge::LoadResult PetDeviceBridge::Load() {
    if (loaded_) return LoadResult::StorageError;
    State candidate{};
    uint64_t highest = 0;
    int best = -1;
    bool corrupt = false, failed = false;
    for (unsigned slot = 0; slot < 2; ++slot) {
        Blob b{};
        const auto read = storage_.Read(slot, &b);
        if (read == BlobReadStatus::Error) { failed = true; continue; }
        if (read == BlobReadStatus::NotFound) continue;
        State decoded{};
        uint64_t gen = 0;
        if (!Decode(b, &decoded, &gen)) { corrupt = true; continue; }
        if (gen > highest) {
            candidate = decoded;
            highest = gen;
            best = static_cast<int>(slot);
        }
    }
    if (failed) return LoadResult::StorageError;
    if (best < 0 && corrupt) return LoadResult::Corrupt;
    state_ = candidate;
    generation_ = highest;
    active_slot_ = best;
    loaded_ = true;
    return best >= 0 ? LoadResult::Ready : LoadResult::Empty;
}

bool PetDeviceBridge::Save(const State& next) {
    if (generation_ == UINT64_MAX) return false;
    Blob bytes{};
    if (!Encode(next, generation_ + 1, &bytes)) return false;
    const unsigned slot = active_slot_ == 0 ? 1u : 0u;
    if (!storage_.Write(slot, bytes)) return false;
    state_ = next;
    active_slot_ = static_cast<int>(slot);
    ++generation_;
    return true;
}

PetDeviceBridge::AppendResult PetDeviceBridge::AppendCommitted(const PetEvent& e) {
    if (!loaded_) return AppendResult::StorageError;
    if (!e.id || !ServerAction(e.action)) return AppendResult::Unsupported;
    for (size_t i = 0; i < state_.count; ++i)
        if (state_.pending[i].event_id == e.id)
            return state_.pending[i].action == e.action
                ? AppendResult::AlreadyPending : AppendResult::Unsupported;
    if (state_.count == kCapacity || state_.next_sequence == kMaxSeq)
        return AppendResult::Full;
    State next = state_;
    next.pending[next.count++] = {e.id, next.next_sequence, e.action};
    ++next.next_sequence;
    return Save(next) ? AppendResult::Queued : AppendResult::StorageError;
}

bool PetDeviceBridge::Peek(Record* output) const {
    if (!loaded_ || !output || !state_.count) return false;
    *output = state_.pending[0];
    return true;
}

PetDeviceBridge::AckResult PetDeviceBridge::Acknowledge(const ServerAck& ack) {
    if (!loaded_ || state_.count == 0) return AckResult::NoPending;
    const Record& front = state_.pending[0];
    if (ack.sequence != front.sequence || ack.event_id != front.event_id)
        return AckResult::Mismatch;
    if (!ack.accepted || ack.server_version == 0)
        return AckResult::ServerRejected;
    State next = state_;
    for (size_t i = 1; i < next.count; ++i)
        next.pending[i - 1] = next.pending[i];
    next.pending[--next.count] = {};
    return Save(next) ? AckResult::Confirmed : AckResult::StorageError;
}

bool PetDeviceBridge::EncodePostBody(const Record& r, const char* device,
                                     char* output, size_t capacity) {
    const char* name = ServerAction(r.action);
    if (!output || !capacity || !name || !ValidId(device) ||
        !r.event_id || !r.sequence || r.sequence > kMaxSeq) return false;
    const int written = std::snprintf(output, capacity,
        "{\"events\":[{\"schema_version\":\"1.0\",\"message_id\":\"%s:%016" PRIx64
        "\",\"device_id\":\"%s\",\"seq\":%" PRIu64
        ",\"type\":\"pet.event\",\"payload\":{\"event\":\"%s\"}}]}",
        device, r.event_id, device, r.sequence, name);
    return written > 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace kid_app
