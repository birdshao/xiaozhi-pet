#include "kid_app/activity_list.h"
#include "kid_app/refresh_planner.h"
#include "kid_app/scene_model.h"
#include "kid_app/sound_view.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace kid_app;

namespace {
SceneModel GameModel(const GameView& view) {
    SceneModel m{};
    m.kind = SceneKind::Game;
    m.game = view;
    return m;
}

int Ink(const GameView& g) {
    MonoFrame f;
    SceneComposer::Render(GameModel(g), f);
    int n = 0;
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x) n += f.IsBlack(x, y) ? 1 : 0;
    return n;
}

int Diff(const GameView& a, const GameView& b) {
    MonoFrame fa, fb;
    SceneComposer::Render(GameModel(a), fa);
    SceneComposer::Render(GameModel(b), fb);
    int n = 0;
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x) n += fa.IsBlack(x, y) != fb.IsBlack(x, y) ? 1 : 0;
    return n;
}

void CleanCorners(const GameView& g) {
    MonoFrame f;
    SceneComposer::Render(GameModel(g), f);
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            if (SceneComposer::InStatusBox(x, y)) assert(!f.IsBlack(x, y));
    for (int y = MonoFrame::kHeight - 12; y < MonoFrame::kHeight; ++y)
        for (int x : {0, 1, 2, 3, MonoFrame::kWidth - 4, MonoFrame::kWidth - 1}) assert(!f.IsBlack(x, y));
}
}  // namespace

int main() {
    // Activity rows: ids and pictures of this session, valid and reachable.
    static_assert(ActivityTableValid(), "table");
    for (GameChoice id : {GameChoice::Parrot, GameChoice::Songs, GameChoice::ToothBrush,
                          GameChoice::HandWash}) {
        const Activity* a = FindActivity(id);
        assert(a && SoundArtValid(a->art) && a->needs != ActivityNeeds::Motion);
    }
    assert(FindActivity(GameChoice::Parrot)->group == ActivityGroup::Play);
    assert(FindActivity(GameChoice::Songs)->group == ActivityGroup::Play);
    assert(FindActivity(GameChoice::ToothBrush)->group == ActivityGroup::Care);
    assert(FindActivity(GameChoice::HandWash)->group == ActivityGroup::Care);
    assert(static_cast<uint8_t>(GameChoice::Parrot) == 31 && static_cast<uint8_t>(GameChoice::HandWash) == 34);

    // Art ranges never overlap: no picture id is valid for two sessions (a
    // sound card must never draw as an apple), tone clip ids stay above them.
    for (uint32_t id = 1; id < 0x2000; ++id) {  // composite PlayArt (0x2000..) are options too, by design
        const uint16_t art = static_cast<uint16_t>(id);
        const int owners = (GameArtIsOption(art) ? 1 : 0) + (MicArtValid(art) ? 1 : 0) +
                           (CareArtValid(art) ? 1 : 0) + (PlayArtValid(art) ? 1 : 0) +
                           (SoundArtValid(art) ? 1 : 0);
        assert(owners <= 1);
    }
    assert(SoundArtValid(701) && !PlayArtValid(701) && !CareArtValid(701));
    assert(!SoundArtValid(401) && PlayArtValid(401));

    // Every card picture draws something.
    for (uint16_t art = 701; art <= 705; ++art) {
        assert(SoundArtValid(art));
        GameView g;
        g.question_art = art;
        g.option_count = 1;
        g.options[0] = art;
        assert(Ink(g) > Ink(GameView{}));
    }

    // Parrot: each phase is its own picture; the refresh planner sees the change.
    std::vector<GameView> parrot;
    for (ParrotPhase p : {ParrotPhase::Ready, ParrotPhase::Recording, ParrotPhase::Playing})
        parrot.push_back(ParrotGameView({p, ParrotHint::None}));
    parrot.push_back(ParrotGameView({ParrotPhase::Ready, ParrotHint::TooShort}));
    for (size_t i = 0; i < parrot.size(); ++i) {
        assert(parrot[i].sound == kSoundParrot && Ink(parrot[i]) > 2000);
        CleanCorners(parrot[i]);
        for (size_t j = i + 1; j < parrot.size(); ++j) assert(parrot[i] != parrot[j] && Diff(parrot[i], parrot[j]) > 50);
    }
    // V9 S1: every voice card (and the home slot) is its own picture, in the
    // Ready, Recording and Playing phases; the voice number travels in `sc`.
    for (ParrotPhase ph : {ParrotPhase::Ready, ParrotPhase::Recording, ParrotPhase::Playing}) {
        std::vector<GameView> cards;
        for (uint8_t c = 0; c < ParrotSession::kChoices; ++c) {
            if (c == ParrotSession::kHomeChoice && ph != ParrotPhase::Ready) continue;
            cards.push_back(ParrotGameView({ph, ParrotHint::None, c}));
            assert(cards.back().sc == c && Ink(cards.back()) > 2000);
            CleanCorners(cards.back());
        }
        for (size_t i = 0; i < cards.size(); ++i)
            for (size_t j = i + 1; j < cards.size(); ++j) assert(Diff(cards[i], cards[j]) > 300);
    }
    // Ready shows the choice row (one mark per voice plus home): moving the
    // choice changes the row, not just the big card.
    assert(Diff(ParrotGameView({ParrotPhase::Ready, ParrotHint::None, 0}),
                ParrotGameView({ParrotPhase::Ready, ParrotHint::None, 5})) > 1500);

    RefreshPlanner planner;
    planner.Completed(GameModel(parrot[0]), RefreshMode::Full, true);
    assert(planner.Plan(GameModel(parrot[1])) == RefreshMode::Partial);

    // Songs: list cursor, timer choice, sway steps, no-songs picture.
    SongView list;
    list.phase = SongPhase::List;
    list.count = 4;
    SongView next = list;
    next.selected = 2;
    assert(SongGameView(list) != SongGameView(next) && Diff(SongGameView(list), SongGameView(next)) > 50);
    // A full list (kMaxSongs = 16 dots, 40 px apart): every dot is drawn, the
    // cursor on the last one is visible, and neighbours do not touch the ring.
    {
        static_assert(kMaxSongs > 12, "the list draws more than the old 12 dots");
        SongView full;
        full.phase = SongPhase::List;
        full.count = static_cast<uint8_t>(kMaxSongs);
        const int pitch = 640 / static_cast<int>(kMaxSongs);
        const int x0 = 400 - pitch * (static_cast<int>(kMaxSongs) - 1) / 2;
        assert(pitch >= 26 + 8 + 4);  // ring radius + neighbour dot + a gap
        for (uint8_t sel = 0; sel < kMaxSongs; ++sel) {
            full.selected = sel;
            MonoFrame f;
            SceneComposer::Render(GameModel(SongGameView(full)), f);
            for (int i = 0; i < static_cast<int>(kMaxSongs); ++i) {
                const int x = x0 + i * pitch;
                assert(f.IsBlack(x, 392));                       // every dot is there
                if (i != sel) assert(!f.IsBlack(x, 392 - 20));   // only the chosen one is ringed
                if (i == sel) assert(f.IsBlack(x, 392 - 26 + 3));
            }
            if (sel > 0) {
                SongView prev = full;
                prev.selected = static_cast<uint8_t>(sel - 1);
                assert(Diff(SongGameView(prev), SongGameView(full)) > 50);
            }
        }
        // More options than the list holds never draw past the last dot.
        SongView over = full;
        over.count = 200;
        assert(Diff(SongGameView(over), SongGameView(full)) == 0);
    }
    SongView timer;
    timer.phase = SongPhase::Timer;
    timer.kind = SongKind::Lullaby;
    timer.timer_minutes = 10;
    SongView timer30 = timer;
    timer30.timer_minutes = 30;
    assert(SongGameView(timer).sd == 1 && SongGameView(timer30).sd == 3);
    assert(Diff(SongGameView(timer), SongGameView(timer30)) > 50);
    SongView play;
    play.phase = SongPhase::Playing;
    play.kind = SongKind::Lullaby;
    play.timer_minutes = 20;
    std::vector<GameView> sway;
    for (uint8_t s = 0; s < 4; ++s) {
        play.sway = s;
        sway.push_back(SongGameView(play));
        CleanCorners(sway.back());
    }
    assert(sway[0] != sway[1] && sway[1] != sway[2] && Diff(sway[0], sway[2]) > 50);
    assert(Diff(sway[1], sway[3]) == 0);  // centre twice: the same picture
    SongView none;
    none.phase = SongPhase::NoSongs;
    none.missing = SongMissing::NoSongs;
    SongView nocard = none;
    nocard.missing = SongMissing::NoCard;
    assert(Ink(SongGameView(none)) > 2000 && Diff(SongGameView(none), SongGameView(nocard)) > 50);
    CleanCorners(SongGameView(none));
    CleanCorners(SongGameView(nocard));
    for (SongPhase p : {SongPhase::List, SongPhase::Timer, SongPhase::Loading}) {
        SongView v = play;
        v.phase = p;
        CleanCorners(SongGameView(v));
    }

    // Routines: the sand moves, praise is its own picture, brush and wash differ.
    RoutineView r;
    r.kind = RoutineKind::Brush;
    r.phase = RoutinePhase::Running;
    r.steps = 10;
    int last = -1;
    for (uint8_t step = 0; step <= 10; ++step) {
        r.step = step;
        const GameView g = RoutineGameView(r);
        CleanCorners(g);
        if (step) {
            RoutineView before = r;
            before.step = static_cast<uint8_t>(step - 1);
            const int moved = Diff(RoutineGameView(before), g);
            if (moved == 0) std::cerr << "step " << int(step) << " moved " << moved << std::endl;
            assert(moved > 0 && (step == 10 || moved > 20));
        }
        last = Ink(g);
    }
    assert(last > 0);
    RoutineView ready = r;
    ready.phase = RoutinePhase::Ready;
    ready.step = 0;
    RoutineView praise = r;
    praise.phase = RoutinePhase::Praise;
    praise.step = 10;
    assert(RoutineGameView(ready) != RoutineGameView(praise) && Diff(RoutineGameView(ready), RoutineGameView(praise)) > 100);
    CleanCorners(RoutineGameView(ready));
    CleanCorners(RoutineGameView(praise));
    RoutineView wash = ready;
    wash.kind = RoutineKind::Wash;
    wash.steps = 2;
    assert(RoutineGameView(wash).sound == kSoundWash && Diff(RoutineGameView(ready), RoutineGameView(wash)) > 50);
    CleanCorners(RoutineGameView(wash));

    // A quiz / motion view is untouched by the new fields.
    GameView quiz;
    quiz.option_count = 3;
    assert(quiz.sound == 0 && quiz.sa == 0 && quiz.sd == 0);

    std::cout << "sound view tests passed\n";
    return 0;
}
