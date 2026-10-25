#pragma once

#include "kid_app/pet_controller.h"
#include <array>
#include <cstddef>

namespace kid_app {

// Deterministic, allocation-free FIFO. Single-owner/task only.
// Interrupts, other FreeRTOS tasks, network and voice callbacks must
// enqueue through an OS-safe port/adapter, never call this simultaneously.
template <size_t Capacity>
class PetEventQueue {
    static_assert(Capacity > 0, "A pet event queue cannot be empty");
public:
    bool Push(const PetEvent& event) {
        if (event.id == 0 || size_ == Capacity) return false;
        events_[tail_] = event;
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        return true;
    }

    bool Pop(PetEvent* result) {
        if (!result || size_ == 0) return false;
        *result = events_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    size_t Size() const { return size_; }
    bool Empty() const { return size_ == 0; }
    bool Full() const { return size_ == Capacity; }

    void Clear() {
        head_ = 0;
        tail_ = 0;
        size_ = 0;
    }

private:
    std::array<PetEvent, Capacity> events_{};
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t size_ = 0;
};

} // namespace kid_app
