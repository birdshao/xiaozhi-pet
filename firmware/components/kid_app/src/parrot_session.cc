#include "kid_app/parrot_session.h"

namespace kid_app {

const char* ParrotHintName(ParrotHint hint) {
    switch (hint) {
        case ParrotHint::None: return "none";
        case ParrotHint::Quiet: return "quiet";
        case ParrotHint::LowBattery: return "low_battery";
        case ParrotHint::Busy: return "busy";
        case ParrotHint::TooShort: return "too_short";
    }
    return "none";
}

void ParrotSession::Bind(int16_t* buffer, size_t capacity) {
    Wipe();
    buffer_ = buffer;
    capacity_ = buffer ? capacity : 0;
    phase_ = ParrotPhase::Idle;
}

// The writes go through a volatile pointer so the compiler cannot drop the
// wipe as a dead store (the buffer is never read again afterwards).
void ParrotSession::Wipe() {
    if (buffer_) {
        volatile int16_t* p = buffer_;
        for (size_t i = 0; i < capacity_; ++i) p[i] = 0;
    }
    length_ = 0;
    out_samples_ = 0;
    step_q16_ = 0;
    gain_q8_ = 256;
}

bool ParrotSession::BufferClean() const {
    for (size_t i = 0; i < capacity_; ++i)
        if (buffer_[i] != 0) return false;
    return length_ == 0 && out_samples_ == 0;
}

bool ParrotSession::Open(uint32_t now_ms, const ParrotContext& c, ParrotOutput* out) {
    ParrotOutput o;
    if (!buffer_ || capacity_ < kMaxSamples || Active()) {
        if (out) *out = o;
        return false;
    }
    if (c.quiet_hours) o.hint = ParrotHint::Quiet;
    else if (c.low_battery) o.hint = ParrotHint::LowBattery;
    else if (c.voice_active) o.hint = ParrotHint::Busy;
    if (o.hint != ParrotHint::None) {
        if (out) *out = o;
        return false;
    }
    Wipe();
    phase_ = ParrotPhase::Ready;
    hint_ = ParrotHint::None;
    last_activity_ms_ = now_ms;
    o.redraw = true;
    if (out) *out = o;
    return true;
}

ParrotOutput ParrotSession::Stop(bool leave) {
    ParrotOutput o;
    o.stop_mic = phase_ == ParrotPhase::Recording;
    o.leave = leave;
    Wipe();
    phase_ = leave ? ParrotPhase::Idle : ParrotPhase::Ready;
    hint_ = ParrotHint::None;
    return o;
}

ParrotOutput ParrotSession::Interrupt() {
    if (phase_ == ParrotPhase::Idle) return ParrotOutput{};
    return Stop(true);
}

void ParrotSession::PlaybackRefused() {
    if (phase_ != ParrotPhase::Playing) return;
    Wipe();
    phase_ = ParrotPhase::Ready;
}

ParrotOutput ParrotSession::Hold(bool down, uint32_t now_ms, const ParrotContext& c, uint32_t seed) {
    ParrotOutput o;
    if (down) {
        if (phase_ != ParrotPhase::Ready) return o;  // already recording / playing
        // Re-checked at every press: quiet hours may begin while the screen is open.
        if (c.quiet_hours || c.low_battery || c.voice_active || c.audio_busy) {
            o.hint = c.quiet_hours ? ParrotHint::Quiet
                                   : c.low_battery ? ParrotHint::LowBattery : ParrotHint::Busy;
            if (c.quiet_hours || c.low_battery) {  // leave: nothing to do here
                const ParrotHint why = o.hint;
                o = Stop(true);
                o.hint = why;
                return o;
            }
            hint_ = o.hint;
            o.redraw = true;
            return o;
        }
        Wipe();
        voice_ = (seed & 1u) ? ParrotVoice::Chipmunk : ParrotVoice::Parrot;
        phase_ = ParrotPhase::Recording;
        hint_ = ParrotHint::None;
        record_start_ms_ = now_ms;
        last_activity_ms_ = now_ms;
        o.start_mic = true;
        o.redraw = true;
        return o;
    }
    if (phase_ != ParrotPhase::Recording) return o;
    return Release(now_ms);
}

ParrotOutput ParrotSession::Release(uint32_t now_ms) {
    ParrotOutput o;
    o.stop_mic = true;
    last_activity_ms_ = now_ms;
    if (length_ < kMinSamples) {  // a tap, not a sentence
        Wipe();
        phase_ = ParrotPhase::Ready;
        hint_ = ParrotHint::TooShort;
        o.hint = ParrotHint::TooShort;
        o.redraw = true;
        return o;
    }
    // Normalise: boost a quiet child up to 3x, never past the peak cap.
    uint32_t peak = 1;
    for (size_t i = 0; i < length_; ++i) {
        const int32_t v = buffer_[i];
        const uint32_t a = static_cast<uint32_t>(v < 0 ? -v : v);
        if (a > peak) peak = a;
    }
    uint32_t gain = (static_cast<uint32_t>(kOutPeak) * 256u) / peak;
    if (gain > kMaxGainQ8) gain = kMaxGainQ8;
    gain_q8_ = gain;
    // Resample kInRate -> kOutRate while reading faster: speed and pitch rise together.
    const uint64_t permille = kPermille[static_cast<size_t>(voice_)];
    step_q16_ = static_cast<uint32_t>((permille * kInRate * 65536ull) / (1000ull * kOutRate));
    const uint64_t span = static_cast<uint64_t>(length_ - 1) << 16;
    out_samples_ = static_cast<size_t>(span / step_q16_) + 1;
    phase_ = ParrotPhase::Playing;
    hint_ = ParrotHint::None;
    o.play = true;
    o.redraw = true;
    return o;
}

size_t ParrotSession::Feed(const int16_t* samples, size_t count) {
    if (phase_ != ParrotPhase::Recording || !samples) return 0;
    size_t room = capacity_ < kMaxSamples ? capacity_ : kMaxSamples;
    room = room > length_ ? room - length_ : 0;
    const size_t take = count < room ? count : room;
    for (size_t i = 0; i < take; ++i) buffer_[length_ + i] = samples[i];
    length_ += take;
    return take;
}

ParrotOutput ParrotSession::Input(GameInput input, uint32_t now_ms) {
    (void)now_ms;
    if ((input == GameInput::Up || input == GameInput::Down || input == GameInput::Long) &&
        phase_ == ParrotPhase::Ready)
        return Stop(true);
    // Playing: a turn of the dial stops the voice and leaves too.
    if ((input == GameInput::Up || input == GameInput::Down) && phase_ == ParrotPhase::Playing)
        return Stop(true);
    return ParrotOutput{};
}

ParrotOutput ParrotSession::Tick(uint32_t now_ms) {
    if (phase_ == ParrotPhase::Recording &&
        static_cast<uint32_t>(now_ms - record_start_ms_) >= kMaxRecordMs)
        return Release(now_ms);  // 5 s cap: act as if the key was released
    if (phase_ == ParrotPhase::Ready &&
        static_cast<uint32_t>(now_ms - last_activity_ms_) >= kIdleExitMs)
        return Stop(true);
    return ParrotOutput{};
}

void ParrotSession::Scrub(int16_t* block, size_t count) {
    if (!block) return;
    volatile int16_t* p = block;
    for (size_t i = 0; i < count; ++i) p[i] = 0;
}

size_t ParrotSession::Render(size_t offset, int16_t* out, size_t max) {
    if (phase_ != ParrotPhase::Playing || !out || offset >= out_samples_) return 0;
    const size_t n = out_samples_ - offset < max ? out_samples_ - offset : max;
    for (size_t k = 0; k < n; ++k) {
        const uint64_t pos = static_cast<uint64_t>(offset + k) * step_q16_;
        const size_t i = static_cast<size_t>(pos >> 16);
        const int32_t frac = static_cast<int32_t>(pos & 0xFFFFu);
        const int32_t a = buffer_[i];
        const int32_t b = i + 1 < length_ ? buffer_[i + 1] : a;
        int32_t v = a + (((b - a) * frac) >> 16);
        v = static_cast<int32_t>((static_cast<int64_t>(v) * gain_q8_) >> 8);
        if (v > kOutPeak) v = kOutPeak;
        if (v < -kOutPeak) v = -kOutPeak;
        out[k] = static_cast<int16_t>(v);
    }
    if (offset + n >= out_samples_) {
        // The whole take has been handed over: nothing of it stays in RAM.
        Wipe();
        phase_ = ParrotPhase::Ready;
    }
    return n;
}

}  // namespace kid_app
