#include "kid_app/care_phrases.h"

namespace kid_app {
namespace {
using L = CareLine;
constexpr int8_t kAny = -1;
constexpr int8_t kCurious = 0, kGentle = 1, kLively = 2;

// Reviewed copy. One line per row: tools/care_phrases.py parses this table
// (id, line, arg, temperament, text) for the TTS checklist. Ids are stable:
// append only, never renumber or reuse.
constexpr CarePhrase kPhrases[] = {
    // Bedtime diary.
    {1001, L::DiaryOpen, 0, kCurious, "我来念今天的日记啦，今天发现了好多事！"},
    {1002, L::DiaryOpen, 0, kGentle, "今天的日记，我轻轻地念给你听。"},
    {1003, L::DiaryOpen, 0, kLively, "日记时间到！今天可热闹啦！"},
    {1011, L::DiaryFood, 1, kAny, "今天我吃了小鱼。"},
    {1012, L::DiaryFood, 2, kAny, "今天我喝了牛奶。"},
    {1013, L::DiaryFood, 3, kAny, "今天我吃了苹果。"},
    {1014, L::DiaryFood, 4, kAny, "今天我吃了胡萝卜。"},
    {1015, L::DiaryFood, 5, kAny, "今天我吃了小饼干。"},
    {1016, L::DiaryFood, 6, kAny, "今天我吃了饭团。"},
    {1021, L::DiaryBook, 0, kAny, "我们一起读了绘本。"},
    {1022, L::DiaryGame, 0, kAny, "我们一起玩了游戏。"},
    {1031, L::DiaryCare, 1, kAny, "你照顾了我一次，谢谢你。"},
    {1032, L::DiaryCare, 2, kAny, "你照顾了我两次，谢谢你。"},
    {1033, L::DiaryCare, 3, kAny, "你照顾了我三次，谢谢你。"},
    {1034, L::DiaryCare, 4, kAny, "你照顾了我好多次，谢谢你。"},
    {1041, L::DiaryHabit, 1, kAny, "你今天自己刷牙了，真棒。"},
    {1042, L::DiaryHabit, 2, kAny, "你今天认真洗手了，真棒。"},
    {1043, L::DiaryHabit, 3, kAny, "你今天把玩具收好了，真棒。"},
    {1044, L::DiaryHabit, 4, kAny, "你今天好好喝水了，真棒。"},
    {1045, L::DiaryHabit, 5, kAny, "你今天自己穿衣服了，真棒。"},
    {1046, L::DiaryHabit, 6, kAny, "你今天吃了蔬菜，真棒。"},
    {1047, L::DiaryHabit, 7, kAny, "你今天按时睡觉了，真棒。"},
    {1048, L::DiaryHabit, 8, kAny, "你今天看了书，真棒。"},
    {1051, L::DiaryQuiet, 0, kAny, "今天我们安安静静的，也很好。"},
    {1061, L::DiaryClose, 0, kCurious, "明天我们再去发现新东西吧，晚安。"},
    {1062, L::DiaryClose, 0, kGentle, "今天过得真好，抱抱你，晚安。"},
    {1063, L::DiaryClose, 0, kLively, "明天还要一起玩哦，晚安！"},
    // Good morning.
    {1101, L::MorningHello, 0, kCurious, "早上好！今天会有什么新发现呢？"},
    {1102, L::MorningHello, 0, kGentle, "早上好呀，睡得好吗？"},
    {1103, L::MorningHello, 0, kLively, "早上好！我们起床啦！"},
    {1111, L::MorningFeel, 1, kAny, "今天有点热，记得多喝水。"},
    {1112, L::MorningFeel, 2, kAny, "今天有点冷，要穿暖和一点。"},
    {1113, L::MorningFeel, 3, kAny, "今天空气有点干，记得喝水哦。"},
    {1114, L::MorningFeel, 4, kAny, "今天湿湿的，像要下雨呢。"},
    {1115, L::MorningFeel, 5, kAny, "今天不冷也不热，真舒服。"},
    // Festivals.
    {1121, L::FestivalHello, 1, kAny, "新年快乐！我戴上了新年帽子。"},
    {1122, L::FestivalHello, 2, kAny, "过年好！祝你新年快乐！"},
    {1123, L::FestivalHello, 3, kAny, "元宵节快乐！我们一起看灯笼吧。"},
    {1124, L::FestivalHello, 4, kAny, "儿童节快乐！今天是你的节日！"},
    {1125, L::FestivalHello, 5, kAny, "端午节快乐！我们一起吃粽子吧。"},
    {1126, L::FestivalHello, 6, kAny, "中秋节快乐！月亮好圆呀。"},
    {1127, L::FestivalHello, 7, kAny, "生日快乐！今天是你的生日！"},
    // Good habits.
    {1131, L::HabitRemind, 1, kAny, "今天也要自己刷牙哦。"},
    {1132, L::HabitRemind, 2, kAny, "今天也要认真洗手哦。"},
    {1133, L::HabitRemind, 3, kAny, "今天也要把玩具收好哦。"},
    {1134, L::HabitRemind, 4, kAny, "今天也要好好喝水哦。"},
    {1135, L::HabitRemind, 5, kAny, "今天也要自己穿衣服哦。"},
    {1136, L::HabitRemind, 6, kAny, "今天也要吃蔬菜哦。"},
    {1137, L::HabitRemind, 7, kAny, "今天也要按时睡觉哦。"},
    {1138, L::HabitRemind, 8, kAny, "今天也要看看书哦。"},
    {1141, L::HabitTicked, 0, kAny, "打勾啦！你做到了！"},
    {1142, L::HabitAlready, 0, kAny, "今天已经打过勾啦，明天再来。"},
    // Feeding.
    {1151, L::FoodReact, 0, kAny, "谢谢你，好吃！"},
    {1152, L::FoodReact, 1, kAny, "哇，我喜欢这个！"},
    {1153, L::FoodReact, 2, kAny, "这是我最爱吃的！谢谢你！"},
    {1161, L::FeedPrompt, 0, kAny, "今天想喂我吃什么呀？"},
    // Body care sessions.
    {1171, L::BathStart, 0, kAny, "洗澡啦，轻轻摇一摇，搓出泡泡。"},
    {1172, L::BathRinse, 0, kAny, "泡泡好多！把我倒过来冲一冲。"},
    {1173, L::BathDone, 0, kAny, "洗得香香的，谢谢你！"},
    {1181, L::BrushStart, 0, kAny, "拨轮来回转，帮我梳梳毛。"},
    {1182, L::BrushDone, 0, kAny, "毛毛顺顺的，真舒服。"},
    {1191, L::TickleStart, 0, kAny, "轻轻摇一摇，挠挠我的痒痒。"},
    {1192, L::TickleDone, 0, kAny, "哈哈哈，好痒呀！"},
    {1201, L::LullStart, 0, kAny, "把我轻轻扣过来，或者放下，我要睡觉啦。"},
    {1202, L::LullDone, 0, kAny, "呼……我睡着啦。"},
    // Bedtime ritual and rewards.
    {1211, L::TidyStart, 0, kAny, "睡觉前，我们把玩具收进箱子吧。"},
    {1212, L::TidyDone, 0, kAny, "玩具都回家啦，真整齐！"},
    {1221, L::StickerNew, 0, kAny, "你得到了一张新贴纸！"},
    {1222, L::DressDone, 0, kAny, "我好看吗？谢谢你帮我打扮！"},
    {1231, L::GentlePlease, 0, kAny, "轻一点点哦。"},
};
constexpr size_t kCount = sizeof(kPhrases) / sizeof(kPhrases[0]);

constexpr bool UniqueIds() {
    for (size_t i = 0; i < kCount; ++i) {
        if (kPhrases[i].id < 1000 || kPhrases[i].id > 1999) return false;
        for (size_t j = 0; j < i; ++j)
            if (kPhrases[j].id == kPhrases[i].id) return false;
    }
    return true;
}
static_assert(UniqueIds(), "care phrase ids: unique, 1000..1999");
}  // namespace

Feel FeelOf(int16_t temp_dc, uint8_t humidity_pct, bool valid) {
    if (!valid) return Feel::Unknown;
    if (temp_dc >= 280) return Feel::Hot;
    if (temp_dc <= 160) return Feel::Cold;
    if (humidity_pct < 30) return Feel::Dry;
    if (humidity_pct > 75) return Feel::Humid;
    return Feel::Comfy;
}

size_t CarePhraseCount() { return kCount; }
const CarePhrase& CarePhraseAt(size_t i) { return kPhrases[i < kCount ? i : 0]; }
const CarePhrase* FindCarePhrase(uint16_t id) {
    for (const auto& p : kPhrases)
        if (p.id == id) return &p;
    return nullptr;
}

uint16_t CarePhraseId(CareLine line, uint8_t arg, Temperament t) {
    const int8_t want = static_cast<int8_t>(t);
    uint16_t shared = 0;
    for (const auto& p : kPhrases) {
        if (p.line != line || p.arg != arg) continue;
        if (p.temperament == want) return p.id;
        if (p.temperament == kAny && !shared) shared = p.id;
    }
    return shared;
}

DiaryPage MakeDiaryPage(const CareStore& care, const MemoryStore& memory, uint32_t day) {
    DiaryPage page{};
    page.day = day;
    page.habit = care.GetHabit().habit;
    const CareStore::Today c = care.TodayOn(day);
    // An undated today (clock never known) still is "today" for the diary.
    if (!day || c.day == day || !c.day) {
        page.last_food = c.last_food;
        page.cares = c.Cares();
        page.habit_done = c.habit_done;
    }
    const MemoryStore::Day& m = memory.Today();
    if (!day || !m.day || m.day == day) {
        page.books = m.books;
        page.games = m.games;
    }
    return page;
}

uint8_t CareBucket(uint16_t cares) {
    return static_cast<uint8_t>(cares >= 4 ? 4 : cares);
}

size_t ComposeDiary(const DiaryPage& page, Temperament t, uint16_t* out, size_t max) {
    if (!out || !max) return 0;
    size_t n = 0;
    auto add = [&](CareLine line, uint8_t arg) {
        const uint16_t id = CarePhraseId(line, arg, t);
        if (id && n < max) out[n++] = id;
    };
    add(L::DiaryOpen, 0);
    if (page.Empty()) {
        add(L::DiaryQuiet, 0);
    } else {
        if (page.last_food) add(L::DiaryFood, page.last_food);
        if (page.books) add(L::DiaryBook, 0);
        if (page.games) add(L::DiaryGame, 0);
        if (page.cares) add(L::DiaryCare, CareBucket(page.cares));
        if (page.habit_done && page.habit != Habit::None)
            add(L::DiaryHabit, static_cast<uint8_t>(page.habit));
    }
    add(L::DiaryClose, 0);
    return n;
}

size_t ComposeMorning(Temperament t, Feel feel, Festival festival, Habit habit, uint16_t* out,
                      size_t max) {
    if (!out || !max) return 0;
    size_t n = 0;
    auto add = [&](CareLine line, uint8_t arg) {
        const uint16_t id = CarePhraseId(line, arg, t);
        if (id && n < max) out[n++] = id;
    };
    add(L::MorningHello, 0);
    if (feel != Feel::Unknown) add(L::MorningFeel, static_cast<uint8_t>(feel));
    if (festival != Festival::None) add(L::FestivalHello, static_cast<uint8_t>(festival));
    if (habit != Habit::None) add(L::HabitRemind, static_cast<uint8_t>(habit));
    return n;
}

}  // namespace kid_app
