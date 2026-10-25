#pragma once

#include "kid_app/pet_controller.h"

#include <array>
#include <cstddef>

namespace kid_app {

// Fixed-memory FIFO. Single-owner only: NOT thread-safe or ISR-safe.
// OS callbacks must send events through the FreeRTOS port instead.
template <size_t Capacity>
class PetEventQueue {
    static_assert(Capacity > 0, "Queue capacity must be nonzero");
public:
    bool Push(const PetEvent& event) {
        if (event.id == 0 || count_ == Capacity) return false;
        items_[tail_] = event;
        tail_ = (tail_ + 1) % Capacity;
        ++count_;
        return true;
    }
    bool Pop(PetEvent* value) {
        if (!value || count_ == 0) return false;
        *value = items_[head_];
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }
    bool Empty() const { return count_ == 0; }
    bool Full() const { return count_ == Capacity; }
    size_t Size() const { return count_; }
    void Clear() { head_ = tail_ = count_ = 0; }

private:
    std::array<PetEvent, Capacity> items_{};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
};

}  // namespace kid_app
