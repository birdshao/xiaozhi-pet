#include "kid_app/pet_atomic_ledger.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>

namespace kid_app {
namespace {
using Ledger = PetAtomicLedger;
using Blob = Ledger::Store::Blob;
constexpr size_t kPetOffset = 28;
constexpr size_t kQueueOffset = kPetOffset + PetCheckpointCodec::kBytes;
constexpr size_t kEntryBytes = 24;
constexpr uint64_t kMaxSeq = static_cast<uint64_t>(INT64_MAX);

void Put16(Blob& b, size_t p, uint16_t n) {
    b[p] = static_cast<uint8_t>(n);
    b[p+1] = static_cast<uint8_t>(n >> 8);
}
void Put32(Blob& b, size_t p, uint32_t n) {
    for (size_t i=0;i<4;++i) b[p+i] = static_cast<uint8_t>(n >> (8*i));
}
void Put64(Blob& b, size_t p, uint64_t n) {
    for (size_t i=0;i<8;++i) b[p+i] = static_cast<uint8_t>(n >> (8*i));
}
uint16_t Get16(const Blob& b, size_t p) {
    return uint16_t(b[p]) | (uint16_t(b[p+1]) << 8);
}
uint32_t Get32(const Blob& b, size_t p) {
    uint32_t n=0;
    for (size_t i=0;i<4;++i) n |= uint32_t(b[p+i]) << (8*i);
    return n;
}
uint64_t Get64(const Blob& b, size_t p) {
    uint64_t n=0;
    for (size_t i=0;i<8;++i) n |= uint64_t(b[p+i]) << (8*i);
    return n;
}
uint32_t Crc32(const Blob& b) {
    uint32_t crc=0xFFFFFFFFu;
    for (size_t i=0;i<b.size()-4;++i) {
        crc ^= b[i];
        for (unsigned j=0;j<8;++j)
            crc = (crc>>1) ^ ((crc&1u) ? 0xEDB88320u : 0);
    }
    return ~crc;
}
}  // namespace

bool PetAtomicLedger::IsProjectable(PetAction action) {
    switch (action) {
        case PetAction::Feed:
        case PetAction::Pickup:
        case PetAction::VoiceBegin:
        case PetAction::VoiceEnd:
        case PetAction::QuietBegin:
        case PetAction::QuietEnd:
            return true;
        default:
            // Story actions have no book_id in PetEvent, so cannot safely
            // reproduce an approved page on the Python server.
            return false;
    }
}

bool PetAtomicLedger::ValidId(const char* id) {
    if (!id) return false;
    const size_t n = std::strlen(id);
    if (n<3 || n>63) return false;
    for (size_t i=0;i<n;++i) {
        const char c=id[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
              (c>='0'&&c<='9')||(i && (c=='_'||c=='-'))))
            return false;
    }
    return true;
}

bool PetAtomicLedger::Encode(const PetCheckpoint& pet,
                             const PetDeviceBridge::State& q,
                             uint64_t generation, Blob* output) {
    if (!output || generation==0 || q.count>PetDeviceBridge::kCapacity ||
        q.next_sequence==0 || q.next_sequence>kMaxSeq ||
        q.next_sequence<=q.count) return false;

    PetCheckpointCodec::Blob pet_blob{};
    if (!PetCheckpointCodec::Encode(pet,generation,&pet_blob)) return false;

    Blob b{};
    b[0]='K'; b[1]='P'; b[2]='A'; b[3]='J';
    Put16(b,4,1);
    Put16(b,6,static_cast<uint16_t>(b.size()));
    Put64(b,8,generation);
    Put64(b,16,q.next_sequence);
    Put16(b,24,q.count);
    for (size_t i=0;i<pet_blob.size();++i) b[kPetOffset+i]=pet_blob[i];

    for (size_t i=0;i<q.count;++i) {
        const auto& r=q.pending[i];
        if (!r.event_id || !IsProjectable(r.action) ||
            r.sequence != q.next_sequence-q.count+i) return false;
        for (size_t j=0;j<i;++j)
            if (q.pending[j].event_id==r.event_id) return false;
        const size_t p=kQueueOffset+kEntryBytes*i;
        Put64(b,p,r.event_id);
        Put64(b,p+8,r.sequence);
        b[p+16]=static_cast<uint8_t>(r.action);
    }
    Put32(b,b.size()-4,Crc32(b));
    *output=b;
    return true;
}

bool PetAtomicLedger::Decode(const Blob& b, PetCheckpoint* pet,
                             PetDeviceBridge::State* queue,
                             uint64_t* generation) {
    if (!pet || !queue || !generation ||
        b[0]!='K'||b[1]!='P'||b[2]!='A'||b[3]!='J' ||
        Get16(b,4)!=1 || Get16(b,6)!=b.size() ||
        b[26] || b[27] ||
        Get32(b,b.size()-4)!=Crc32(b)) return false;

    const uint64_t gen=Get64(b,8);
    PetCheckpointCodec::Blob pet_blob{};
    for (size_t i=0;i<pet_blob.size();++i) pet_blob[i]=b[kPetOffset+i];
    PetCheckpoint candidate{};
    uint64_t nested_gen=0;
    if (!PetCheckpointCodec::Decode(pet_blob,&candidate,&nested_gen) ||
        gen==0 || gen!=nested_gen) return false;

    PetDeviceBridge::State q{};
    q.next_sequence=Get64(b,16);
    q.count=Get16(b,24);
    if (q.count>PetDeviceBridge::kCapacity) return false;
    for (size_t i=0;i<PetDeviceBridge::kCapacity;++i) {
        const size_t p=kQueueOffset+kEntryBytes*i;
        if (i<q.count) {
            q.pending[i].event_id=Get64(b,p);
            q.pending[i].sequence=Get64(b,p+8);
            q.pending[i].action=static_cast<PetAction>(b[p+16]);
            for (size_t j=17;j<kEntryBytes;++j) if (b[p+j]) return false;
        } else {
            for (size_t j=0;j<kEntryBytes;++j) if (b[p+j]) return false;
        }
    }
    Blob check{};
    if (!Encode(candidate,q,gen,&check) || check!=b) return false;
    *pet=candidate;
    *queue=q;
    *generation=gen;
    return true;
}

PetAtomicLedger::LoadResult PetAtomicLedger::Load() {
    if (initialized_ || storage_empty_) return LoadResult::StorageError;
    bool corrupt=false, failed=false;
    uint64_t best_gen=0;
    int best_slot=-1;
    PetCheckpoint best_pet{};
    PetDeviceBridge::State best_queue{};

    for (unsigned slot=0;slot<2;++slot) {
        Blob b{};
        auto result=store_.Read(slot,&b);
        if (result==BlobReadStatus::Error) {failed=true;continue;}
        if (result==BlobReadStatus::NotFound) continue;
        PetCheckpoint p{};
        PetDeviceBridge::State q{};
        uint64_t gen=0;
        if (!Decode(b,&p,&q,&gen)) {corrupt=true;continue;}
        if (gen>best_gen) {
            best_gen=gen;
            best_pet=p;
            best_queue=q;
            best_slot=static_cast<int>(slot);
        }
    }
    if (failed) return LoadResult::StorageError;
    if (best_slot<0) {
        if (corrupt) return LoadResult::Corrupt;
        storage_empty_=true;
        return LoadResult::Empty;
    }

    // Never restore an interrupted microphone/speech transaction as active.
    if (best_pet.snapshot.mode==PetMode::Listening ||
        best_pet.snapshot.mode==PetMode::Speaking) {
        best_pet.snapshot.mode=PetMode::Idle;
        best_pet.snapshot.mood=PetMood::Neutral;
    }
    if (!controller_.RestoreCheckpoint(best_pet)) return LoadResult::Corrupt;
    queue_=best_queue;
    generation_=best_gen;
    active_slot_=best_slot;
    initialized_=true;
    return LoadResult::Ready;
}

bool PetAtomicLedger::Commit(const PetCheckpoint& pet,
                             const PetDeviceBridge::State& queue) {
    if (generation_==std::numeric_limits<uint64_t>::max()) return false;
    Blob next{};
    if (!Encode(pet,queue,generation_+1,&next)) return false;
    const unsigned slot=active_slot_==0 ? 1u : 0u;
    if (!store_.Write(slot,next)) return false;
    // Save order: commit single storage blob before acknowledging in memory.
    if (!controller_.RestoreCheckpoint(pet)) return false;  // verified by Encode
    queue_=queue;
    ++generation_;
    active_slot_=static_cast<int>(slot);
    initialized_=true;
    return true;
}

bool PetAtomicLedger::InitializeFresh() {
    if (!storage_empty_ || initialized_) return false;
    return Commit(controller_.ExportCheckpoint(),PetDeviceBridge::State{});
}

bool PetAtomicLedger::ImportLegacyForFreshServer(
        const PetCheckpoint& checkpoint, uint64_t legacy_generation) {
    if (!storage_empty_ || initialized_ || legacy_generation==0 ||
        legacy_generation==std::numeric_limits<uint64_t>::max())
        return false;
    // Preflight must verify remote cursor==0 and old bridge has no events.
    const uint64_t previous=generation_;
    generation_=legacy_generation;
    if (!Commit(checkpoint,PetDeviceBridge::State{})) {
        generation_=previous;
        return false;
    }
    return true;
}

CommitResult PetAtomicLedger::ApplyDurably(const PetEvent& event,
                                           PetEffects* effects) {
    if (!effects) return CommitResult::Rejected;
    *effects=PetEffects{};
    if (!initialized_) return CommitResult::StorageError;
    const PetCheckpoint before=controller_.ExportCheckpoint();
    PetEffects proposed_effects{};
    const auto outcome=controller_.Apply(event,&proposed_effects);
    if (outcome==PetResult::Duplicate) return CommitResult::Duplicate;
    if (outcome==PetResult::Rejected) return CommitResult::Rejected;

    PetDeviceBridge::State pending=queue_;
    bool can_commit=true;
    if (IsProjectable(event.action)) {
        if (pending.count==PetDeviceBridge::kCapacity ||
            pending.next_sequence>=kMaxSeq) {
            can_commit=false;
        } else {
            pending.pending[pending.count++] = {
                event.id,pending.next_sequence,event.action
            };
            ++pending.next_sequence;
        }
    }

    // One physical NVS commit contains both updated controller checkpoint
    // and outbound message. A write failure restores the entire live state.
    if (!can_commit || !Commit(controller_.ExportCheckpoint(),pending)) {
        if (!controller_.RestoreCheckpoint(before)) return CommitResult::StorageError;
        return CommitResult::StorageError;
    }
    *effects=proposed_effects;
    return CommitResult::Committed;
}

bool PetAtomicLedger::Peek(PetDeviceBridge::Record* out) const {
    if (!initialized_ || !out || !queue_.count) return false;
    *out=queue_.pending[0];
    return true;
}

PetAtomicLedger::AckResult PetAtomicLedger::Acknowledge(
        const PetDeviceBridge::ServerAck& ack, const char* device_id) {
    if (!initialized_ || !queue_.count) return AckResult::Empty;
    if (!ValidId(device_id) || !ack.message_id) return AckResult::Mismatch;
    const auto& first=queue_.pending[0];
    char expected[96]{};
    const int n=std::snprintf(expected,sizeof(expected),"%s:%016" PRIx64,
                              device_id,first.event_id);
    if (n<1 || static_cast<size_t>(n)>=sizeof(expected) ||
        ack.sequence!=first.sequence ||
        std::strcmp(expected,ack.message_id)!=0) return AckResult::Mismatch;
    if (!ack.accepted || ack.server_version==0) return AckResult::Rejected;
    PetDeviceBridge::State next=queue_;
    for (size_t i=1;i<next.count;++i) next.pending[i-1]=next.pending[i];
    next.pending[--next.count]={};
    if (!Commit(controller_.ExportCheckpoint(),next)) return AckResult::StorageError;
    return AckResult::Confirmed;
}

}  // namespace kid_app
