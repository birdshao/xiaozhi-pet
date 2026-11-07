# V8 养成与照顾（会话①）

> 本文先落地“活动列表”共享入口一节；其余章节随后续提交补全。

## 活动列表（给其他会话的共享入口）

首页“玩”（拨轮选 Game 项按下）打开的游戏菜单，以及 V8 新增的“照顾”菜单，卡片都来自
同一张数据表：`firmware/components/kid_app/include/kid_app/activity_list.h` 的
`kActivities[]`。`GameMenu`（`motion_game.h`）按分组（`ActivityGroup::Play` /
`ActivityGroup::Care`）读这张表，一次显示 3 张卡片，窗口随高亮滑动，按下返回该行的
`GameChoice`。

### 追加一个活动（3 步）

1. **追加 id**：在 `enum class GameChoice` 末尾追加一个值（从 13 起，已占用 1–12）。
   只追加，不改号、不复用。
2. **追加一行**：在 `kActivities[]` 里对应分组的“append ... rows above this line”注释
   上方加一行 `{GameChoice::Xxx, ActivityGroup::Play, 图片id, ActivityNeeds::..., "xxx"}`。
   - 图片必须是 `SceneComposer` 已经会画的 id（`GameArt` / `MicArt` / `CareArt`）；
     新图片先追加到对应枚举并在 `scene_model.cc` 的卡片绘制里实现。
   - `needs`：`Motion`（家长关掉体感时板子会拒绝）、`Mic`、`None`。
   - 编译期 `static_assert(ActivityTableValid())` 检查 id 唯一、图片已知。
3. **追加分派**：在 `scripts/p1_integrate.py` 的 `HandleMenuOutput()` 里，在
   “V8 activity list ... add their dispatch branch here” 注释处加一个
   `else if (out.chosen == kid_app::GameChoice::Xxx) { ... }` 分支。
   没有分派分支的行被按下时，板子记 `PET_GAME_REFUSED reason=no_handler` 并留在菜单里，
   不会误启动别的游戏。

表是头文件内 `constexpr`，追加行**不需要**改任何编译脚本。卡片顺序就是表的顺序；
`ActivityIndexOf(id)` 给出某个 id 在本组里的卡片下标（用于被拒绝后重新打开菜单停在原卡片）。

主机测试：`tests/cpp/test_motion_game.cc` 的 `TestActivityList()` 会遍历 Play 组的每一行，
确认菜单能选到、按下返回的 id 正确。追加行后这个测试自动覆盖新行。
