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
    // ---- V9 TTS drafts (ids >= 1300): an adult listens before packing ----
    // C1 the line of the day after a bath / brush / tickle (arg = CheerArg).
    {1301, L::BathDone, 1, kCurious, "泥点点都不见啦，我变干净了！"},
    {1302, L::BathDone, 2, kCurious, "咦，泡泡一搓，泥点就跑掉啦！"},
    {1303, L::BathDone, 3, kCurious, "干干净净的，我又可以去探险啦！"},
    {1304, L::BathDone, 1, kGentle, "洗得暖暖的，好舒服呀。"},
    {1305, L::BathDone, 2, kGentle, "谢谢你轻轻帮我洗澡。"},
    {1306, L::BathDone, 3, kGentle, "香香的，抱一下好吗？"},
    {1307, L::BathDone, 1, kLively, "哗啦啦，洗完啦，我闪闪发亮！"},
    {1308, L::BathDone, 2, kLively, "泡泡大战结束啦，好好玩！"},
    {1309, L::BathDone, 3, kLively, "干净啦！我们再玩一会儿吧！"},
    {1311, L::BrushDone, 1, kCurious, "毛结都解开啦，毛毛好顺！"},
    {1312, L::BrushDone, 2, kCurious, "咦，梳一梳，毛毛亮晶晶的。"},
    {1313, L::BrushDone, 3, kCurious, "一个毛结都没有啦，你真细心！"},
    {1314, L::BrushDone, 1, kGentle, "梳得轻轻的，好舒服，谢谢你。"},
    {1315, L::BrushDone, 2, kGentle, "毛毛软软的，像云朵一样。"},
    {1316, L::BrushDone, 3, kGentle, "慢慢梳，真舒服呀。"},
    {1317, L::BrushDone, 1, kLively, "唰唰唰，我变成蓬蓬团团啦！"},
    {1318, L::BrushDone, 2, kLively, "梳好啦，我要去跑一圈！"},
    {1319, L::BrushDone, 3, kLively, "毛毛顺顺的，我最神气啦！"},
    {1321, L::TickleDone, 1, kCurious, "哈哈，你怎么知道我这里痒？"},
    {1322, L::TickleDone, 2, kCurious, "嘻嘻，羽毛好神奇呀！"},
    {1323, L::TickleDone, 3, kCurious, "哈哈哈，羽毛找到痒痒啦！"},
    {1324, L::TickleDone, 1, kGentle, "嘻嘻，好痒，好开心。"},
    {1325, L::TickleDone, 2, kGentle, "呵呵，轻轻的，好好玩。"},
    {1326, L::TickleDone, 3, kGentle, "嘻嘻，你挠得刚刚好。"},
    {1327, L::TickleDone, 1, kLively, "哈哈哈哈，好痒好痒！"},
    {1328, L::TickleDone, 2, kLively, "哇哈哈，我要笑翻啦！"},
    {1329, L::TickleDone, 3, kLively, "哈哈，下次换我来挠你！"},
    {1331, L::TickleLaugh, 0, kCurious, "哈哈哈！那是我最怕痒的脚丫！"},
    {1332, L::TickleLaugh, 0, kGentle, "嘻嘻嘻，肚子最怕痒啦，哈哈！"},
    {1333, L::TickleLaugh, 0, kLively, "哇哈哈哈！耳朵最怕痒啦！"},
    {1334, L::TickleSeek, 0, kAny, "痒痒在哪里？转拨轮把羽毛移过去，再轻轻摇一摇。"},
    // G5 the diary names today's game (arg = growth memory id).
    {1401, L::DiaryGameNamed, 1, kAny, "今天我们一起答了题。"},
    {1402, L::DiaryGameNamed, 2, kAny, "今天我们一起吹了蜡烛。"},
    {1403, L::DiaryGameNamed, 3, kAny, "今天我们一起拍了手。"},
    {1404, L::DiaryGameNamed, 4, kAny, "今天我们一起玩了滚球。"},
    {1405, L::DiaryGameNamed, 5, kAny, "今天我们一起玩了躲猫猫。"},
    {1421, L::DiaryGameNamed, 21, kAny, "今天我们一起玩了翻牌。"},
    {1422, L::DiaryGameNamed, 22, kAny, "今天我们一起玩了六张翻牌。"},
    {1423, L::DiaryGameNamed, 23, kAny, "今天我们一起走了迷宫。"},
    {1424, L::DiaryGameNamed, 24, kAny, "今天我们一起摇了骰子。"},
    {1425, L::DiaryGameNamed, 25, kAny, "今天我们一起摇了果树。"},
    {1426, L::DiaryGameNamed, 26, kAny, "今天我们一起玩了木头人。"},
    {1427, L::DiaryGameNamed, 27, kAny, "今天我们一起玩了白天和黑夜。"},
    {1428, L::DiaryGameNamed, 28, kAny, "今天我们一起认了形状。"},
    {1429, L::DiaryGameNamed, 29, kAny, "今天我们一起数了数。"},
    {1431, L::DiaryGameNamed, 31, kAny, "今天我们一起找了不一样。"},
    {1432, L::DiaryGameNamed, 32, kAny, "今天我们帮宝宝找到了妈妈。"},
    {1433, L::DiaryGameNamed, 33, kAny, "今天我们一起排了大小。"},
    {1434, L::DiaryGameNamed, 34, kAny, "今天我们一起找了规律。"},
    {1435, L::DiaryGameNamed, 35, kAny, "今天我们一起分了类。"},
    {1436, L::DiaryGameNamed, 36, kAny, "今天我们一起找了影子。"},
    {1437, L::DiaryGameNamed, 37, kAny, "今天我们一起认了左和右。"},
    {1438, L::DiaryGameNamed, 38, kAny, "今天我们一起看了时钟。"},
    {1439, L::DiaryGameNamed, 39, kAny, "今天我们一起看天气穿了衣服。"},
    {1441, L::BoredGone, 0, kAny, "玩得好开心，不无聊啦！"},
    // S4 the lullaby in the bedtime ritual.
    {1451, L::SongAsk, 0, kAny, "要听摇篮曲吗？要就选月亮，不要就选团团睡觉。"},
    // C4 seasonal foods (DiaryFood arg = food picture: 7, 8 zongzi, 9 mooncake).
    {1341, L::DiaryFood, 7, kAny, "今天我吃了饺子。"},
    {1342, L::DiaryFood, 8, kAny, "今天我吃了粽子。"},
    {1343, L::DiaryFood, 9, kAny, "今天我吃了月饼。"},
    {1344, L::WishHit, 0, kCurious, "咦？你怎么知道我想吃这个！"},
    {1345, L::WishHit, 0, kGentle, "你怎么知道我想吃这个呀，谢谢你！"},
    {1346, L::WishHit, 0, kLively, "哇！你怎么知道我想吃这个！"},
    // C3 TuanTuan grows up (arg = the stage reached).
    {1361, L::GrowMorning, 1, kAny, "早上好！我好像长大了一点点，耳朵变长啦。"},
    {1362, L::GrowMorning, 2, kAny, "早上好！我又长大了，现在是大团团啦！"},
    {1363, L::GrowMorning, 3, kAny, "早上好！我长成团团队长啦，谢谢你一直照顾我！"},
    {1366, L::GrowDiary, 1, kAny, "今天我好像长大了一点点。"},
    {1367, L::GrowDiary, 2, kAny, "今天我变成大团团啦。"},
    {1368, L::GrowDiary, 3, kAny, "今天我变成团团队长啦，是你陪我长大的。"},
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
        // Only a dated today names its game (undated: maybe another day's).
        page.last_game = day && m.day == day && m.games ? m.last_game : 0;
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
        if (page.last_food) {  // V9 C4: food 8 is named by its season
            const bool treat = page.last_food == static_cast<uint8_t>(Food::FestiveTreat);
            add(L::DiaryFood, treat && page.treat == Treat::Mooncake ? 9 : page.last_food);
        }
        if (page.books) add(L::DiaryBook, 0);
        if (page.games) {
            // V9 G5: the game by name when it is a built-in one.
            const uint16_t named = page.last_game && page.last_game <= 0xFF
                                       ? CarePhraseId(L::DiaryGameNamed,
                                                      static_cast<uint8_t>(page.last_game), t)
                                       : 0;
            if (named && n < max) out[n++] = named;
            else add(L::DiaryGame, 0);
        }
        if (page.cares) add(L::DiaryCare, CareBucket(page.cares));
        if (page.habit_done && page.habit != Habit::None)
            add(L::DiaryHabit, static_cast<uint8_t>(page.habit));
    }
    if (page.grew) add(L::GrowDiary, page.grew);  // V9 C3, just before good night
    add(L::DiaryClose, 0);
    return n;
}

uint16_t BoredReliefClip(uint8_t bored_level, Temperament t) {
    return bored_level >= 2 ? CarePhraseId(L::BoredGone, 0, t) : 0;
}

size_t ComposeMorning(Temperament t, Feel feel, Festival festival, Habit habit, uint16_t* out,
                      size_t max, uint8_t grew) {
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
    if (grew) add(L::GrowMorning, grew);  // V9 C3
    return n;
}

}  // namespace kid_app
