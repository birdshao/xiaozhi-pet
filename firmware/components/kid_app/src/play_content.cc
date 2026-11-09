#include "kid_app/play_content.h"

#include <algorithm>

namespace kid_app {
namespace {
constexpr uint16_t U(PlayArt art) { return static_cast<uint16_t>(art); }

// Menu order: age, then level.
const PlayGameInfo kGames[kPlayGameCount] = {
    {PlayGame::Shapes, 28, "shapes", 3, 1, false, GameKind::MatchShape},
    {PlayGame::Counting, 29, "counting", 3, 1, false, GameKind::Count},
    {PlayGame::DayNight, 27, "day_night", 3, 1, true, GameKind::MatchShape},
    {PlayGame::Memory4, 21, "memory4", 3, 2, false, GameKind::MatchShape},
    {PlayGame::MamaBaby, 32, "mama_baby", 3, 2, false, GameKind::MamaBaby},
    {PlayGame::Classify, 35, "classify", 3, 2, false, GameKind::Classify},
    {PlayGame::Tree, 25, "shake_tree", 3, 3, true, GameKind::MatchShape},
    {PlayGame::Shadow, 36, "shadow", 4, 1, false, GameKind::Shadow},
    {PlayGame::Statue, 26, "statue", 4, 1, true, GameKind::MatchShape},
    {PlayGame::Weather, 39, "weather", 4, 2, false, GameKind::Weather},
    {PlayGame::LeftRight, 37, "left_right", 4, 2, false, GameKind::LeftRight},
    {PlayGame::SpotDiff, 31, "spot_diff", 4, 2, false, GameKind::SpotDiff},
    {PlayGame::Dice, 24, "shake_dice", 4, 3, true, GameKind::MatchShape},
    {PlayGame::Memory6, 22, "memory6", 4, 3, false, GameKind::MatchShape},
    {PlayGame::SizeOrder, 33, "size_order", 5, 1, false, GameKind::SizeOrder},
    {PlayGame::Pattern, 34, "pattern", 5, 2, false, GameKind::Pattern},
    {PlayGame::Maze, 23, "maze", 5, 2, true, GameKind::MatchShape},
    {PlayGame::Clock, 38, "clock", 5, 3, false, GameKind::Clock},
};

// One round: `correct_art` is put on card `slot`, the others fill the
// remaining cards in order. Every card has its generic tone (the firmware
// says nothing in words until a reviewed recording exists).
GameRound Round(GameKind kind, uint16_t question, uint16_t correct_art, uint16_t d1, uint16_t d2,
                uint8_t slot) {
    GameRound r{};
    r.kind = kind;
    r.option_count = d2 ? 3 : 2;
    r.correct = std::min<uint8_t>(slot, static_cast<uint8_t>(r.option_count - 1));
    r.question_art = question;
    const uint16_t others[2] = {d1, d2};
    uint8_t next = 0;
    for (uint8_t i = 0; i < r.option_count; ++i)
        r.option_art[i] = i == r.correct ? correct_art : others[next++];
    r.prompt_clip = kGameTonePrompt;
    for (uint8_t i = 0; i < r.option_count; ++i) r.option_clip[i] = GameArtTone(r.option_art[i]);
    return r;
}

constexpr size_t kBankMax = 48;
struct Bank {
    GameRound rounds[kBankMax];
    size_t count = 0;
    void Add(const GameRound& r) {
        if (count < kBankMax) rounds[count++] = r;
    }
};

GameRound ClockRound(uint8_t hour24, bool half, uint8_t salt) {
    const PlayArt right = ClockActivity(hour24, half);
    const PlayArt all[4] = {PlayArt::Eat, PlayArt::Sleep, PlayArt::Play, PlayArt::Bath};
    uint16_t others[3];
    uint8_t n = 0;
    for (PlayArt a : all)
        if (a != right) others[n++] = U(a);
    const uint8_t i = salt % 3;
    return Round(GameKind::Clock, PlayClockArt(hour24, half), U(right), others[i],
                 others[(i + 1) % 3], static_cast<uint8_t>(salt % 3));
}

struct Banks {
    Bank spot, mama, order, pattern, classify, shadow, side, clock, weather;

    Banks() {
        BuildSpot();
        BuildMama();
        BuildOrder();
        BuildPattern();
        BuildClassify();
        BuildShadow();
        BuildSide();
        BuildClock();
        BuildWeather();
    }

    // 24 rows of three shapes: position and the replaced shape vary.
    void BuildSpot() {
        for (uint8_t i = 0; i < 24; ++i) {
            const uint8_t s0 = i % 4, s1 = static_cast<uint8_t>((i / 4 + i) % 4),
                          s2 = static_cast<uint8_t>((i / 2 + 2 * i / 3) % 4), idx = i % 3,
                          delta = static_cast<uint8_t>((i / 3) % 3);
            spot.Add(Round(GameKind::SpotDiff, PlaySpotArt(s0, s1, s2, idx, delta),
                           U(PlayArt::Pos1) + idx, U(PlayArt::Pos1) + (idx + 1) % 3,
                           U(PlayArt::Pos1) + (idx + 2) % 3, idx));
        }
    }

    // Six animals, four distractor pairs each.
    void BuildMama() {
        const uint16_t mama_art[6] = {41, 42, 43, 44, U(PlayArt::Rabbit), U(PlayArt::Duck)};
        const uint16_t baby_art[6] = {U(PlayArt::BabyCat),    U(PlayArt::BabyDog),
                                      U(PlayArt::BabyBird),   U(PlayArt::BabyFish),
                                      U(PlayArt::BabyRabbit), U(PlayArt::BabyDuck)};
        for (uint8_t m = 0; m < 6; ++m) {
            for (uint8_t k = 0; k < 4; ++k) {
                const uint8_t a = static_cast<uint8_t>((m + 1 + k) % 6),
                              b = static_cast<uint8_t>((m + 2 + k) % 6);
                if (a == m || b == m || a == b) continue;
                mama.Add(Round(GameKind::MamaBaby, mama_art[m], baby_art[m], baby_art[a],
                               baby_art[b], static_cast<uint8_t>((m + k) % 3)));
            }
        }
    }

    // A strip of three sizes with one hidden: which size fits?
    void BuildOrder() {
        for (uint8_t shape = 0; shape < 5; ++shape) {
            for (uint8_t dir = 0; dir < 2; ++dir) {
                for (uint8_t hidden = 0; hidden < 3; ++hidden) {
                    const uint8_t want = dir ? static_cast<uint8_t>(2 - hidden) : hidden;
                    const uint8_t o1 = static_cast<uint8_t>((want + 1) % 3),
                                  o2 = static_cast<uint8_t>((want + 2) % 3);
                    order.Add(Round(GameKind::SizeOrder, PlayRankArt(shape, dir != 0, hidden),
                                    PlaySizedArt(shape, want), PlaySizedArt(shape, o1),
                                    PlaySizedArt(shape, o2),
                                    static_cast<uint8_t>((shape + dir + hidden) % 3)));
                }
            }
        }
    }

    // The strip shows the start of a repeating pattern, the child picks the
    // next one. type 0: A B A B ? -> A; type 1: A A B A A ? -> B;
    // type 2: A B C A B ? -> C.
    void BuildPattern() {
        auto add = [&](uint8_t type, uint8_t a, uint8_t b, uint8_t c, uint8_t answer) {
            uint8_t others[4], n = 0;
            for (uint8_t s = 0; s < 5; ++s)
                if (s != answer) others[n++] = s;
            const uint8_t r = static_cast<uint8_t>((a + 2 * b + type) % 4);
            pattern.Add(Round(GameKind::Pattern, PlayPatternArt(type, a, b, c),
                              kPlayShapeArt[answer], kPlayShapeArt[others[r]],
                              kPlayShapeArt[others[(r + 1) % 4]],
                              static_cast<uint8_t>((a + b + type) % 3)));
        };
        for (uint8_t a = 0; a < 5; ++a)
            for (uint8_t b = 0; b < 5; ++b)
                if (a != b) add(0, a, b, 0, a);
        for (uint8_t a = 0; a < 5; ++a)
            for (uint8_t b = a + 1; b < 5; ++b) add(1, a, b, 0, b);
        for (uint8_t a = 0; a < 5; ++a) {
            for (uint8_t b = a + 1; b < 5; ++b) {
                uint8_t c = static_cast<uint8_t>((b + 1) % 5);
                while (c == a || c == b) c = static_cast<uint8_t>((c + 1) % 5);
                add(2, a, b, c, c);
            }
        }
    }

    // Which one can be eaten / cannot be eaten?
    void BuildClassify() {
        const uint16_t edible[4] = {U(PlayArt::Apple), U(PlayArt::Carrot), U(PlayArt::Cake),
                                    U(PlayArt::Bread)};
        const uint16_t inedible[4] = {U(PlayArt::Shoe), U(PlayArt::Hat), U(PlayArt::Cup), 31};
        static const uint8_t kPairs[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
        for (uint8_t i = 0; i < 4; ++i) {
            for (uint8_t k = 0; k < 3; ++k) {
                const uint8_t* pair = kPairs[(i * 2 + k) % 6];
                classify.Add(Round(GameKind::Classify, U(PlayArt::EatYes), edible[i],
                                   inedible[pair[0]], inedible[pair[1]],
                                   static_cast<uint8_t>((i + k) % 3)));
                classify.Add(Round(GameKind::Classify, U(PlayArt::EatNo), inedible[i],
                                   edible[pair[0]], edible[pair[1]],
                                   static_cast<uint8_t>((i + k + 1) % 3)));
            }
        }
    }

    // Nine solid pictures, the wrong shadows rotate.
    void BuildShadow() {
        const uint16_t base[9] = {4, 5, 3, U(PlayArt::Apple), U(PlayArt::Carrot),
                                  U(PlayArt::Bread), U(PlayArt::Shoe), U(PlayArt::Hat),
                                  U(PlayArt::Cup)};
        for (uint8_t i = 0; i < 27; ++i) {
            const uint8_t q = i % 9, step = static_cast<uint8_t>(1 + i / 9 * 2 + (i % 2));
            const uint8_t a = static_cast<uint8_t>((q + step) % 9),
                          b = static_cast<uint8_t>((q + step + 3) % 9);
            if (a == q || b == q || a == b) continue;
            shadow.Add(Round(GameKind::Shadow, base[q], PlayShadowArt(base[q]),
                             PlayShadowArt(base[a]), PlayShadowArt(base[b]),
                             static_cast<uint8_t>(i % 3)));
        }
    }

    // Left, right, middle: an arrow with a shape, the box that has the shape
    // on that side.
    void BuildSide() {
        for (uint8_t shape = 0; shape < 5; ++shape) {
            for (uint8_t dir = 0; dir < 3; ++dir) {
                const uint8_t o1 = static_cast<uint8_t>((dir + 1) % 3),
                              o2 = static_cast<uint8_t>((dir + 2) % 3);
                side.Add(Round(GameKind::LeftRight, PlayArrowArt(dir, shape),
                               PlaySideArt(dir, shape), PlaySideArt(o1, shape),
                               PlaySideArt(o2, shape), static_cast<uint8_t>((shape + dir) % 3)));
            }
        }
        for (uint8_t shape = 0; shape < 5; ++shape) {  // two cards: left or right only
            for (uint8_t dir = 0; dir < 2; ++dir)
                side.Add(Round(GameKind::LeftRight, PlayArrowArt(dir, shape),
                               PlaySideArt(dir, shape),
                               PlaySideArt(static_cast<uint8_t>(1 - dir), shape), 0,
                               static_cast<uint8_t>((shape + dir) % 2)));
        }
    }

    // 24 times of the day and what we do then.
    void BuildClock() {
        static const uint8_t kTimes[24][2] = {
            {7, 0},  {7, 1},  {8, 0},  {8, 1},  {9, 0},  {10, 0}, {11, 0}, {12, 0},
            {12, 1}, {13, 0}, {13, 1}, {14, 0}, {15, 0}, {16, 0}, {17, 0}, {18, 0},
            {18, 1}, {19, 0}, {19, 1}, {20, 0}, {20, 1}, {21, 0}, {21, 1}, {22, 0}};
        for (uint8_t i = 0; i < 24; ++i)
            clock.Add(ClockRound(kTimes[i][0], kTimes[i][1] != 0, i));
    }

    // Weather picture -> clothes that fit it, two other things as distractors.
    void BuildWeather() {
        struct Q {
            uint16_t art;
            uint16_t good[2];
        };
        const Q qs[5] = {{U(PlayArt::Sun), {U(PlayArt::Tshirt), U(PlayArt::Sunglasses)}},
                         {U(PlayArt::Snow), {U(PlayArt::Scarf), U(PlayArt::Gloves)}},
                         {U(PlayArt::Rain), {U(PlayArt::Umbrella), 0}},
                         {U(PlayArt::ThermoHot), {U(PlayArt::Tshirt), U(PlayArt::Sunglasses)}},
                         {U(PlayArt::ThermoCold), {U(PlayArt::Scarf), U(PlayArt::Gloves)}}};
        const uint16_t all[5] = {U(PlayArt::Tshirt), U(PlayArt::Sunglasses), U(PlayArt::Scarf),
                                 U(PlayArt::Gloves), U(PlayArt::Umbrella)};
        for (uint8_t q = 0; q < 5; ++q) {
            uint16_t wrong[5];
            uint8_t n = 0;
            for (uint16_t item : all)
                if (item != qs[q].good[0] && item != qs[q].good[1]) wrong[n++] = item;
            for (uint8_t g = 0; g < 2; ++g) {
                if (!qs[q].good[g]) continue;
                for (uint8_t k = 0; k < 3; ++k) {
                    const uint8_t a = static_cast<uint8_t>((k + q) % n),
                                  b = static_cast<uint8_t>((a + 1 + k % 2) % n);
                    if (a == b) continue;
                    weather.Add(Round(GameKind::Weather, qs[q].art, qs[q].good[g], wrong[a],
                                      wrong[b], static_cast<uint8_t>((q + g + k) % 3)));
                }
            }
        }
    }
};

const Banks& TheBanks() {
    static const Banks banks;
    return banks;
}
}  // namespace

const PlayGameInfo* PlayGames(size_t* count) {
    if (count) *count = kPlayGameCount;
    return kGames;
}

const PlayGameInfo* FindPlayGame(PlayGame game) {
    for (const PlayGameInfo& g : kGames)
        if (g.game == game) return &g;
    return nullptr;
}

bool IsCardGame(PlayGame game) { return game >= PlayGame::Shapes && game <= PlayGame::Weather; }

const GameRound* PlayBank(PlayGame game, size_t* count) {
    const Banks& b = TheBanks();
    const Bank* bank = nullptr;
    switch (game) {
        case PlayGame::Shapes: return BuiltinGameBank(GameKind::MatchShape, count);
        case PlayGame::Counting: return BuiltinGameBank(GameKind::Count, count);
        case PlayGame::SpotDiff: bank = &b.spot; break;
        case PlayGame::MamaBaby: bank = &b.mama; break;
        case PlayGame::SizeOrder: bank = &b.order; break;
        case PlayGame::Pattern: bank = &b.pattern; break;
        case PlayGame::Classify: bank = &b.classify; break;
        case PlayGame::Shadow: bank = &b.shadow; break;
        case PlayGame::LeftRight: bank = &b.side; break;
        case PlayGame::Clock: bank = &b.clock; break;
        case PlayGame::Weather: bank = &b.weather; break;
        default: break;
    }
    if (!bank) {
        if (count) *count = 0;
        return nullptr;
    }
    if (count) *count = bank->count;
    return bank->rounds;
}

PlayArt ClockActivity(uint8_t hour24, bool half) {
    (void)half;
    if (hour24 >= 6 && hour24 <= 8) return PlayArt::Eat;      // breakfast
    if (hour24 >= 9 && hour24 <= 11) return PlayArt::Play;
    if (hour24 == 12) return PlayArt::Eat;                    // lunch
    if (hour24 >= 13 && hour24 <= 14) return PlayArt::Sleep;  // nap
    if (hour24 >= 15 && hour24 <= 17) return PlayArt::Play;
    if (hour24 == 18) return PlayArt::Eat;                    // dinner
    if (hour24 == 19) return PlayArt::Bath;
    return PlayArt::Sleep;                                    // evening and night
}

size_t BuildClockPool(bool clock_ok, uint8_t hour24, uint8_t minute, GameRound out[3]) {
    const uint8_t now_hour = clock_ok && hour24 < 24 ? hour24 : 8;
    const bool now_half = clock_ok && minute >= 30;
    out[0] = ClockRound(now_hour, now_half, now_hour);
    // The others are a morning and an evening hour that differ from now.
    const uint8_t a = now_hour == 8 ? 12 : 8, b = now_hour == 20 ? 13 : 20;
    out[1] = ClockRound(a, false, a);
    out[2] = ClockRound(b, false, b);
    return 3;
}

size_t BuildWeatherPool(bool sensor_ok, int temp_c10, GameRound out[3]) {
    const bool hot = sensor_ok && temp_c10 >= kWeatherHotC10;
    const bool cold = sensor_ok && temp_c10 <= kWeatherColdC10;
    if (hot)
        out[0] = Round(GameKind::Weather, U(PlayArt::ThermoHot), U(PlayArt::Tshirt),
                       U(PlayArt::Scarf), U(PlayArt::Gloves), 0);
    else if (cold)
        out[0] = Round(GameKind::Weather, U(PlayArt::ThermoCold), U(PlayArt::Scarf),
                       U(PlayArt::Tshirt), U(PlayArt::Sunglasses), 1);
    else
        out[0] = Round(GameKind::Weather, U(PlayArt::Rain), U(PlayArt::Umbrella),
                       U(PlayArt::Gloves), U(PlayArt::Sunglasses), 2);
    out[1] = Round(GameKind::Weather, U(PlayArt::Sun), U(PlayArt::Tshirt), U(PlayArt::Scarf),
                   U(PlayArt::Umbrella), 1);
    out[2] = Round(GameKind::Weather, U(PlayArt::Snow), U(PlayArt::Scarf),
                   U(PlayArt::Sunglasses), U(PlayArt::Tshirt), 0);
    return 3;
}

const char* PlayGamePromptText(PlayGame game) {
    switch (game) {
        case PlayGame::Memory4:
        case PlayGame::Memory6: return "找一找，哪两张是一样的？";
        case PlayGame::Maze: return "把团团歪一歪，帮团团找到胡萝卜吧。";
        case PlayGame::Dice: return "摇一摇骰子，数一数有几个点。";
        case PlayGame::Tree: return "摇一摇小树，数一数掉下几个果子。";
        case PlayGame::Statue: return "先动一动，听到“木头人”，就一动也不动。";
        case PlayGame::DayNight: return "天黑啦，把团团扣下来睡觉；天亮啦，再翻回来。";
        case PlayGame::Shapes: return "找一找，哪个和它一样？";
        case PlayGame::Counting: return "数一数，有几个？";
        case PlayGame::SpotDiff: return "两排图里，有一个不一样，是第几个？";
        case PlayGame::MamaBaby: return "哪一个是它的宝宝？";
        case PlayGame::SizeOrder: return "大、中、小排好队，缺的是哪一个？";
        case PlayGame::Pattern: return "接下来是哪一个？";
        case PlayGame::Classify: return "看上面的小图，找出符合它的那一个。";
        case PlayGame::Shadow: return "哪一个是它的影子？";
        case PlayGame::LeftRight: return "箭头指向哪一边，就选哪一边。";
        case PlayGame::Clock: return "现在是这个时间，我们该做什么？";
        case PlayGame::Weather: return "今天天气这样，团团该穿什么？";
        case PlayGame::None: break;
    }
    return "";
}

}  // namespace kid_app
