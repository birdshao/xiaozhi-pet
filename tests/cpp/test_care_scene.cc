// V8 pictures: clothes, room items, festival props and the need bubble on
// the home screen; care / diary / album scenes. ePaper rules: a highlight
// move only touches the card rail, a care step only the picture above it,
// a look change only the pet area above the home rail, and nothing ever
// draws in the status corner boxes. Also the look word and refresh plan.
#include "kid_app/activity_list.h"
#include "kid_app/care_session.h"
#include "kid_app/refresh_planner.h"
#include "kid_app/scene_model.h"

#include <cassert>
#include <cstdio>

using namespace kid_app;

namespace {

int DiffRows(const SceneModel& a, const SceneModel& b, int* top, int* bottom) {
    static MonoFrame fa, fb;
    SceneComposer::Render(a, fa);
    SceneComposer::Render(b, fb);
    int n = 0;
    *top = MonoFrame::kHeight;
    *bottom = -1;
    for (int y = 0; y < MonoFrame::kHeight; ++y) {
        for (int x = 0; x < MonoFrame::kWidth; ++x) {
            if (fa.IsBlack(x, y) == fb.IsBlack(x, y)) continue;
            ++n;
            if (y < *top) *top = y;
            if (y > *bottom) *bottom = y;
        }
    }
    return n;
}

void AssertCornersClear(const SceneModel& m) {
    static MonoFrame f;
    SceneComposer::Render(m, f);
    for (int y = 0; y < SceneComposer::kStatusBottom; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            assert(!SceneComposer::InStatusBox(x, y) || !f.IsBlack(x, y));
}

void TestLookWord() {
    HomeLook look{};
    assert(PackLook(look) == 0 && UnpackLook(0) == look);
    look.worn = {4, 3, 2, 1, 2};
    look.need = 3;
    look.need_level = 3;
    look.festival = 7;
    assert(UnpackLook(PackLook(look)) == look);
    HomeLook bad = look;
    bad.need_level = 9;  // out of range: dropped, never smeared into festival
    assert(UnpackLook(PackLook(bad)).need_level == 0 && UnpackLook(PackLook(bad)).festival == 7);
    // The scene word is unchanged by V8 (the look is not packed).
    SceneModel m{};
    m.look = look;
    assert(PackScene(m) == PackScene(SceneModel{}));
    assert(static_cast<uint8_t>(SceneKind::Care) == 5);
    SceneModel care{};
    care.kind = SceneKind::Care;
    assert(UnpackScene(PackScene(care)).kind == SceneKind::Care);
}

void TestHomeLook() {
    const SceneModel plain{};
    int top = 0, bottom = 0;
    // Every wardrobe item on every slot, each festival, each need level.
    for (uint8_t slot = 0; slot < 5; ++slot) {
        for (uint8_t item = 1; item <= 4; ++item) {
            SceneModel m{};
            m.look.worn[slot] = item;
            AssertCornersClear(m);
            if (DiffRows(plain, m, &top, &bottom))
                assert(bottom < SceneComposer::kRailTop);  // the rail never changes
        }
    }
    for (uint8_t fest = 1; fest <= 7; ++fest) {
        SceneModel m{};
        m.look.festival = fest;
        AssertCornersClear(m);
        assert(DiffRows(plain, m, &top, &bottom) > 0 && bottom < SceneComposer::kRailTop);
    }
    for (uint8_t need = 1; need <= 4; ++need) {
        for (uint8_t level = 0; level <= 3; ++level) {
            SceneModel m{};
            m.look.need = need;
            m.look.need_level = level;
            AssertCornersClear(m);
            const int d = DiffRows(plain, m, &top, &bottom);
            assert((d > 0) == (level >= 2));  // only levels 2..3 show
            if (d) assert(bottom < SceneComposer::kRailTop);
        }
    }
    // Everything at once still keeps the corners and the rail.
    SceneModel full{};
    full.look.worn = {3, 1, 2, 1, 2};
    full.look.need = 1;
    full.look.need_level = 3;
    full.look.festival = 6;
    AssertCornersClear(full);
    assert(DiffRows(plain, full, &top, &bottom) > 0 && bottom < SceneComposer::kRailTop);
    // A sleepy need gives the resting pet sleepy eyes; a voice face wins.
    SceneModel sleepy{};
    sleepy.look.need = 2;
    sleepy.look.need_level = 2;
    SceneModel sleepy_face{};
    sleepy_face.emotion = Emotion::Sleepy;
    sleepy_face.look = sleepy.look;
    assert(DiffRows(sleepy, sleepy_face, &top, &bottom) == 0);
    SceneModel speaking = sleepy;
    speaking.emotion = Emotion::Speaking;
    SceneModel speaking_plain{};
    speaking_plain.emotion = Emotion::Speaking;
    // While speaking the face is the voice face (no sleepy override); the
    // bubble still shows next to it.
    assert(DiffRows(speaking, speaking_plain, &top, &bottom) > 0);
    // The refresh planner sees a look change as a pet-area partial refresh.
    RefreshPlanner planner;
    planner.Completed(plain, RefreshMode::Full, true);
    assert(planner.Plan(full) == RefreshMode::Partial);
    planner.Completed(full, RefreshMode::Partial, true);
    assert(planner.PetPartials() == 1 && planner.RailPartials() == 0);
}

void TestCareScenes() {
    CareContext ctx{};
    ctx.temperament = Temperament::Gentle;
    ctx.stickers = kAllStickers;
    ctx.worn = {1, 1, 1, 0, 0};
    ctx.habit = Habit::EatVeggies;
    ctx.diary.last_food = 6;
    ctx.diary.books = 1;
    ctx.diary.games = 1;
    ctx.diary.cares = 7;
    ctx.diary.habit_done = true;
    int top = 0, bottom = 0;
    for (uint8_t k = 1; k <= static_cast<uint8_t>(CareKind::Album); ++k) {
        CareSession s;
        CareOutput out{};
        assert(s.Start(static_cast<CareKind>(k), ctx, 0, &out));
        SceneModel base{};
        base.kind = SceneKind::Care;
        base.care = s.View();
        AssertCornersClear(base);
        // Highlight moves only touch the card rail.
        if (base.care.option_count > 1) {
            SceneModel moved = base;
            moved.care.highlight = 1;
            assert(DiffRows(base, moved, &top, &bottom) > 0);
            assert(top >= SceneComposer::kGameRailTop);
        }
        // Progress, reactions and the look only touch the picture above it.
        const bool cards = base.care.option_count > 0;
        SceneModel step = base;
        step.care.progress = static_cast<uint8_t>(base.care.progress + 1);
        step.care.hearts = 2;
        step.care.worn[0] = 3;
        AssertCornersClear(step);
        if (DiffRows(base, step, &top, &bottom) > 0 && cards)
            assert(bottom < SceneComposer::kGameRailTop);
    }
    // The album draws every page, earned or not, inside the screen.
    SceneModel album{};
    album.kind = SceneKind::Care;
    album.care.kind = CareKind::Album;
    album.care.pages = kAlbumPages;
    for (uint8_t page = 0; page < kAlbumPages; ++page) {
        album.care.page = page;
        album.care.stickers = 0;
        AssertCornersClear(album);
        SceneModel earned = album;
        earned.care.stickers = kAllStickers;
        AssertCornersClear(earned);
        assert(DiffRows(album, earned, &top, &bottom) > 0);
    }
    // The diary shows only what happened.
    SceneModel quiet{};
    quiet.kind = SceneKind::Care;
    quiet.care.kind = CareKind::Diary;
    SceneModel busy = quiet;
    busy.care.food = 1;
    busy.care.cares = 2;
    assert(DiffRows(quiet, busy, &top, &bottom) > 0);
    // Every care card picture draws inside its card on the game rail too
    // (the care menu is the GameMenu on the Care group).
    for (uint16_t art = 200; art <= 262; ++art) {
        if (!CareArtValid(art)) continue;
        SceneModel g{};
        g.kind = SceneKind::Game;
        g.game.option_count = 3;
        g.game.question_art = static_cast<uint16_t>(GameArt::Heart);
        SceneModel with = g;
        with.game.options[1] = art;
        assert(DiffRows(g, with, &top, &bottom) > 0);
        assert(top >= SceneComposer::kGameRailTop);
    }
    // Planner: the care rail and picture share the game budgets.
    RefreshPlanner planner;
    SceneModel a{};
    a.kind = SceneKind::Care;
    a.care.kind = CareKind::Feed;
    a.care.option_count = 3;
    planner.Completed(a, RefreshMode::Full, true);
    SceneModel b = a;
    b.care.highlight = 2;
    assert(planner.Plan(b) == RefreshMode::Partial);
    planner.Completed(b, RefreshMode::Partial, true);
    assert(planner.RailPartials() == 1 && planner.PetPartials() == 0);
    SceneModel c = b;
    c.care.hearts = 3;
    planner.Completed(c, RefreshMode::Partial, true);
    assert(planner.PetPartials() == 1);
    SceneModel home{};
    assert(planner.Plan(home) == RefreshMode::Full);  // leaving a care scene
}

}  // namespace

int main() {
    TestLookWord();
    TestHomeLook();
    TestCareScenes();
    std::puts("PASS: V8 home look, care / diary / album scenes stay in their regions");
    return 0;
}
