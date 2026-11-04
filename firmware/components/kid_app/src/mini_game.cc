#include "kid_app/mini_game.h"

#include "kid_app/memory_store.h"

#include <algorithm>
#include <cmath>

namespace kid_app {
namespace {
constexpr uint16_t A(GameArt art) { return static_cast<uint16_t>(art); }

GameRound Builtin(GameKind kind, GameArt question, GameArt a, GameArt b, GameArt c,
                  uint8_t correct) {
    GameRound r{};
    r.kind = kind;
    r.option_count = c == GameArt::None ? 2 : 3;
    r.correct = correct;
    r.question_art = A(question);
    r.option_art = {A(a), A(b), A(c)};
    r.prompt_clip = kGameTonePrompt;
    for (uint8_t i = 0; i < r.option_count; ++i) r.option_clip[i] = GameArtTone(r.option_art[i]);
    return r;
}

// Wordless rounds: the question card shows what to look for, the answer
// cards show the same thing among others. The option order is shuffled
// per session, so the position in this table does not matter.
const GameRound kBuiltin[] = {
    Builtin(GameKind::MatchShape, GameArt::Circle, GameArt::Circle, GameArt::Square,
            GameArt::Triangle, 0),
    Builtin(GameKind::MatchShape, GameArt::Star, GameArt::Heart, GameArt::Star, GameArt::None, 1),
    Builtin(GameKind::MatchShape, GameArt::Triangle, GameArt::Square, GameArt::Circle,
            GameArt::Triangle, 2),
    Builtin(GameKind::MatchShape, GameArt::Heart, GameArt::Heart, GameArt::Star, GameArt::Circle,
            0),
    Builtin(GameKind::Count, GameArt::Blocks2, GameArt::Dots1, GameArt::Dots2, GameArt::Dots3, 1),
    Builtin(GameKind::Count, GameArt::Blocks3, GameArt::Dots3, GameArt::Dots1, GameArt::None, 0),
    Builtin(GameKind::Count, GameArt::Blocks1, GameArt::Dots2, GameArt::Dots1, GameArt::Dots4, 1),
    Builtin(GameKind::Count, GameArt::Blocks4, GameArt::Dots2, GameArt::Dots5, GameArt::Dots4, 2),
};

// ---- Built-in tones ----
struct Note {
    uint16_t hz;
    uint16_t ms;
};
struct Motif {
    const Note* notes;
    size_t count;
};
template <size_t N>
constexpr Motif M(const Note (&notes)[N]) {
    return {notes, N};
}

constexpr Note kRight[] = {{523, 90}, {659, 90}, {784, 90}, {1047, 220}};
constexpr Note kTryAgain[] = {{587, 160}, {523, 160}, {587, 220}};
constexpr Note kReveal[] = {{659, 140}, {784, 140}, {659, 240}};
constexpr Note kBye[] = {{784, 120}, {659, 120}, {523, 260}};
constexpr Note kPrompt[] = {{659, 120}, {880, 220}};
// Counting cards say their number as that many beeps.
constexpr Note kBeeps[] = {{880, 110}, {880, 110}, {880, 110}, {880, 110}, {880, 110}};
constexpr Note kCircle[] = {{523, 300}};
constexpr Note kSquare[] = {{392, 140}, {392, 140}};
constexpr Note kTriangle[] = {{659, 100}, {784, 100}, {988, 140}};
constexpr Note kStar[] = {{1175, 80}, {1319, 80}, {1568, 160}};
constexpr Note kHeart[] = {{659, 160}, {587, 220}};
constexpr Note kBallBig[] = {{262, 400}};
constexpr Note kBallSmall[] = {{1047, 120}};
constexpr Note kCat[] = {{784, 120}, {659, 200}};
constexpr Note kDog[] = {{392, 90}, {392, 140}};
constexpr Note kBird[] = {{1568, 70}, {1760, 70}, {1568, 70}};
constexpr Note kFish[] = {{440, 90}, {523, 90}, {440, 160}};
constexpr Note kListen[] = {{523, 120}, {659, 120}};

bool MotifFor(uint16_t clip, Motif* out) {
    switch (clip) {
        case kGameClipRight: *out = M(kRight); return true;
        case kGameClipTryAgain: *out = M(kTryAgain); return true;
        case kGameClipReveal: *out = M(kReveal); return true;
        case kGameClipBye: *out = M(kBye); return true;
        case kGameTonePrompt: *out = M(kPrompt); return true;
        default: break;
    }
    if (clip <= kGameToneBase) return false;
    const uint16_t art = static_cast<uint16_t>(clip - kGameToneBase);
    if (art >= A(GameArt::Dots1) && art <= A(GameArt::Dots5)) {
        *out = {kBeeps, static_cast<size_t>(art - A(GameArt::Dots1) + 1)};
        return true;
    }
    if (art >= A(GameArt::Blocks1) && art <= A(GameArt::Blocks5)) {
        *out = {kBeeps, static_cast<size_t>(art - A(GameArt::Blocks1) + 1)};
        return true;
    }
    switch (static_cast<GameArt>(art)) {
        case GameArt::Circle: *out = M(kCircle); return true;
        case GameArt::Square: *out = M(kSquare); return true;
        case GameArt::Triangle: *out = M(kTriangle); return true;
        case GameArt::Star: *out = M(kStar); return true;
        case GameArt::Heart: *out = M(kHeart); return true;
        case GameArt::BallBig: *out = M(kBallBig); return true;
        case GameArt::BallSmall: *out = M(kBallSmall); return true;
        case GameArt::Cat: *out = M(kCat); return true;
        case GameArt::Dog: *out = M(kDog); return true;
        case GameArt::Bird: *out = M(kBird); return true;
        case GameArt::Fish: *out = M(kFish); return true;
        case GameArt::Listen: *out = M(kListen); return true;
        default: break;
    }
    return false;
}

constexpr size_t kRate = 24000;
constexpr size_t kGapSamples = kRate * 40 / 1000;  // between notes
constexpr float kPi = 3.14159265f;

size_t NoteSamples(const Note& note) { return kRate * note.ms / 1000; }

int16_t ToneSample(const Note& note, size_t i) {
    const size_t n = NoteSamples(note);
    const float t = static_cast<float>(i) / kRate;
    // 8 ms attack, 40 ms release: no clicks at note edges.
    const float env = std::min({1.0f, static_cast<float>(i) / (0.008f * kRate),
                                static_cast<float>(n - i) / (0.040f * kRate)});
    const float w = 2.0f * kPi * note.hz * t;
    const float s = std::sin(w) + 0.25f * std::sin(2.0f * w);
    return static_cast<int16_t>(0.2f * 32767.0f * env * s / 1.25f);
}
}  // namespace

const char* GameKindName(GameKind kind) {
    switch (kind) {
        case GameKind::MatchShape: return "match_shape";
        case GameKind::Count: return "count";
        case GameKind::AnimalSound: return "animal_sound";
        case GameKind::BigSmall: return "big_small";
        case GameKind::EnglishWord: return "english_word";
    }
    return "unknown";
}

const char* GameEndName(GameEnd end) {
    switch (end) {
        case GameEnd::None: return "none";
        case GameEnd::Completed: return "completed";
        case GameEnd::Home: return "home";
        case GameEnd::Idle: return "idle";
        case GameEnd::Interrupted: return "interrupted";
    }
    return "unknown";
}

bool GameRoundValid(const GameRound& r) {
    if (!GameKindValid(static_cast<uint8_t>(r.kind)) || r.option_count < 2 ||
        r.option_count > 3 || r.correct >= r.option_count || r.prompt_clip == 0 ||
        (r.question_art != 0 && !GameArtIsQuestion(r.question_art)))
        return false;
    for (uint8_t i = 0; i < 3; ++i) {
        if (i >= r.option_count) {
            if (r.option_art[i] != 0 || r.option_clip[i] != 0) return false;
            continue;
        }
        if (!GameArtIsOption(r.option_art[i]) || r.option_clip[i] == 0) return false;
        for (uint8_t j = 0; j < i; ++j)
            if (r.option_art[j] == r.option_art[i]) return false;
    }
    return true;
}

size_t BuiltinGameRoundCount() { return sizeof(kBuiltin) / sizeof(kBuiltin[0]); }

const GameRound* BuiltinGameRounds() { return kBuiltin; }

const GameRound& BuiltinGameRound(size_t index) {
    return kBuiltin[index < BuiltinGameRoundCount() ? index : 0];
}

size_t GameToneSamples(uint16_t clip) {
    Motif m{};
    if (!MotifFor(clip, &m)) return 0;
    size_t total = 0;
    for (size_t i = 0; i < m.count; ++i) total += NoteSamples(m.notes[i]) + kGapSamples;
    return total;
}

size_t RenderGameTone(uint16_t clip, size_t offset, int16_t* out, size_t max) {
    Motif m{};
    const size_t total = GameToneSamples(clip);
    if (!MotifFor(clip, &m) || !out || offset >= total) return 0;
    const size_t n = std::min(max, total - offset);
    for (size_t k = 0; k < n; ++k) {
        size_t pos = offset + k;
        int16_t value = 0;
        for (size_t j = 0; j < m.count; ++j) {
            const size_t len = NoteSamples(m.notes[j]);
            if (pos < len) {
                value = ToneSample(m.notes[j], pos);
                break;
            }
            pos -= len;
            if (pos < kGapSamples) break;  // silence between notes
            pos -= kGapSamples;
        }
        out[k] = value;
    }
    return n;
}

// ---- GameSession ----

uint32_t GameSession::Next() {
    // xorshift32: tiny, deterministic, never 0 from a non-zero state.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

bool GameSession::Start(const GameRound* pool, size_t pool_size, uint16_t game_id,
                        uint32_t seed, uint8_t rounds, uint32_t now_ms, GameOutput* out) {
    if (!pool || pool_size == 0 || pool_size > kMaxPool || game_id == 0 || !out) return false;
    for (size_t i = 0; i < pool_size; ++i)
        if (!GameRoundValid(pool[i])) return false;
    *out = {};
    pool_ = pool;
    rng_ = seed ? seed : 0x9E3779B9u;
    const uint8_t count = static_cast<uint8_t>(
        std::min<size_t>(std::max<uint8_t>(1, std::min(rounds, kMaxRounds)), pool_size));
    // Partial Fisher-Yates: `count` distinct rounds in a seed-given order.
    std::array<uint8_t, kMaxPool> index{};
    for (size_t i = 0; i < pool_size; ++i) index[i] = static_cast<uint8_t>(i);
    for (uint8_t i = 0; i < count; ++i) {
        const size_t j = i + Next() % (pool_size - i);
        std::swap(index[i], index[j]);
        order_[i] = index[i];
    }
    result_ = {};
    result_.game_id = game_id;
    result_.rounds = count;
    BeginRound(now_ms, out);
    return true;
}

void GameSession::BeginRound(uint32_t now_ms, GameOutput* out) {
    const GameRound& source = pool_[order_[result_.played]];
    current_ = source;
    // Shuffle the answer cards so the right one is not always in one place.
    std::array<uint8_t, 3> perm = {0, 1, 2};
    for (uint8_t i = source.option_count; i > 1; --i)
        std::swap(perm[i - 1], perm[Next() % i]);
    for (uint8_t i = 0; i < source.option_count; ++i) {
        current_.option_art[i] = source.option_art[perm[i]];
        current_.option_clip[i] = source.option_clip[perm[i]];
        if (perm[i] == source.correct) current_.correct = i;
    }
    phase_ = Phase::Asking;
    feedback_ = GameFeedback::None;
    choice_.Reset(current_.option_count);
    tries_ = 0;
    repeated_ = false;
    last_input_ms_ = now_ms;
    out->redraw = true;
    out->clip = current_.prompt_clip;
}

void GameSession::Finish(GameEnd end, GameOutput* out) {
    phase_ = Phase::Done;
    result_.end = end;
    out->finished = true;
    out->clip = kGameClipBye;
}

void GameSession::Interrupt() {
    if (!Active()) return;
    phase_ = Phase::Done;
    result_.end = GameEnd::Interrupted;
}

GameOutput GameSession::Input(GameInput input, uint32_t now_ms) {
    GameOutput out{};
    if (!Active()) return out;
    last_input_ms_ = now_ms;
    if (input == GameInput::Long) {
        Finish(GameEnd::Home, &out);
        return out;
    }
    // TuanTuan is reacting: nothing to answer (and nothing to win).
    if (phase_ != Phase::Asking) return out;
    if (input == GameInput::Up || input == GameInput::Down) {
        if (!choice_.Move(input == GameInput::Up ? -1 : 1)) return out;
        feedback_ = GameFeedback::None;
        out.redraw = true;
        out.clip = current_.option_clip[choice_.Index()];
        return out;
    }
    out.redraw = true;
    if (choice_.Index() == current_.correct) {
        ++result_.correct;
        phase_ = Phase::Right;
        feedback_ = GameFeedback::Right;
        phase_until_ms_ = now_ms + kRightMs;
        out.right = true;
        out.clip = kGameClipRight;
    } else if (++tries_ >= kMaxTries) {
        phase_ = Phase::Reveal;
        feedback_ = GameFeedback::Reveal;
        choice_.Reset(current_.option_count, current_.correct);
        phase_until_ms_ = now_ms + kRevealMs;
        out.clip = kGameClipReveal;
    } else {
        feedback_ = GameFeedback::TryAgain;
        out.clip = kGameClipTryAgain;
    }
    return out;
}

GameOutput GameSession::Tick(uint32_t now_ms) {
    GameOutput out{};
    if (phase_ == Phase::Asking) {
        if (Reached(now_ms, last_input_ms_ + kIdleExitMs)) {
            Finish(GameEnd::Idle, &out);
        } else if (!repeated_ && Reached(now_ms, last_input_ms_ + kRepeatPromptMs)) {
            repeated_ = true;  // say the question once more, then wait
            out.clip = current_.prompt_clip;
        }
        return out;
    }
    if ((phase_ == Phase::Right || phase_ == Phase::Reveal) && Reached(now_ms, phase_until_ms_)) {
        ++result_.played;
        if (result_.played >= result_.rounds) Finish(GameEnd::Completed, &out);
        else BeginRound(now_ms, &out);
    }
    return out;
}

GameView GameSession::View() const {
    GameView v{};
    if (phase_ == Phase::Idle) return v;
    v.rounds = result_.rounds;
    v.round = static_cast<uint8_t>(std::min<int>(result_.played + 1, result_.rounds));
    v.option_count = current_.option_count;
    v.highlight = choice_.Index();
    v.feedback = feedback_;
    v.question_art = current_.question_art;
    v.options = current_.option_art;
    return v;
}

bool GameCompletedMemoryEvent(const GameResult& result, uint64_t event_id, uint32_t day,
                              MemoryEvent* out) {
    if (!out || !event_id || result.game_id == 0 || result.end != GameEnd::Completed ||
        result.correct == 0)
        return false;
    MemoryEvent e{};
    e.id = event_id;
    e.kind = MemoryEventKind::GameCompleted;
    e.source = MemorySource::Device;
    e.day = day;
    e.option = result.game_id;
    *out = e;
    return true;
}

}  // namespace kid_app
