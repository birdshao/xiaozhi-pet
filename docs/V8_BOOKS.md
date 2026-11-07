# V8：绘本升级（分支绘本、睡前故事模式、团团系列）

会话④“绘本升级”。基础见 [V4_BOOK_READER.md](V4_BOOK_READER.md)、[V4_KPB2_CONTENT_FORMAT.md](V4_KPB2_CONTENT_FORMAT.md)。一切都在 Reader 场景内完成：没有新 `SceneKind`、没有改首页菜单、没有碰记忆存储格式。

硬约束照旧：离线、规则在设备上且确定，大模型不决定任何状态；`Voice`/`Network` 来源不写记忆；不惩罚孩子；BOOT（说话）和 PWR（哄睡）行为不变；朗读只在画面上屏后开始；音频抢占、刷新门不变。

## 1. 玩法

### 1.1 分支绘本（“走左边的路还是右边的路？”）

| 场景 | 拨轮下 | 拨轮上 | 按下 | 长按 |
|---|---|---|---|---|
| 分支页（本页有选路题） | **马上出选路题**（不用等朗读，故事没有选择就走不下去） | 回到来时的那一页 | 再读一遍 | 回首页 |
| 选路中 | 高亮到下一个图标 | 高亮到上一个图标 | 选定，3 秒后**跳到该选项指定的页** | 回首页 |
| 分支书里的普通页 | 去本页的“下一页”（`next`，默认页码+1） | 回到来时的那一页 | 再读一遍 | 回首页 |
| 结局画面 | — | 回到来时的最后一页 | 从第 1 页再读 | 回首页 |

- **“上一页 = 来时的那一页”**：阅读器记一条走过的页的栈。向前走压栈，向上弹栈；线性书里这和“页码−1”完全一样（旧书行为不变）。栈只在“再读一遍/重启”时清空。图是无环的（见 §2.3），所以栈最长 = 页数 + 1。
- 选路题的反馈沿用普通题：被选的图标标出“已选”，念选项名，3 秒后翻页，不放“错了”的声音。选路题**没有对错**（`answer` 不允许）。
- 每条路径走到结局（`next: "end"` 或最后一页之后）在**普通模式**下都算读完：生成 `BookFinished{content_id, source=Device}`，每次打开只一次（睡前模式自动读完不算，见 §1.2）。**多个结局共用同一个书 id。** 中途转头走另一条路是正常的，不影响。
- 喜好题照旧：只来自 `DialChoice`，只有选了 `like` 选项才写；选路题本身也可以不带 like（本批的书都不带）。“再读一遍”回到第 1 页，不算读完。

### 1.2 睡前故事模式

触发：从首页“讲故事”进入，且 RTC 日相是**晚间**（`RhythmScheduler::PhaseAt == DayPhase::Evening`，默认 19:30 起，家长可改日程；RTC 没设时间就不进睡前模式）。纯规则，不联网。

| 睡前模式里 | 行为 |
|---|---|
| 翻页 | 页朗读结束后停 **2.5 秒** 自己翻到下一页（`kBedtimePauseMs`） |
| 题目 | **不出题**，也不写任何喜好记忆 |
| 读完记忆 | 睡前模式**自己读到结局不算读完，不写 `BookFinished`**（孩子可能已经睡着了）；中途转拨轮回到普通模式后读完的，照常算 |
| 分支页 | 没人选，故事沿用**第一个选项**的路。校验器保证第一个选项**不是 restart**，所以这条路一定走到结局、进得了睡觉流程；每本分支书的第一个选项也要是一条完整的好故事 |
| 画面 | 低对比度：4 灰度页的级别 0,1,2,3 → 1,1,2,2（黑变深灰、白变浅灰，不刺眼）；没有灰度帧的页（1-bit 回退）不变 |
| 读完 | 团团的结局画面变成**困倦脸**（`Emotion::Sleepy`），停 **6 秒**（`kBedtimeYawnMs`），然后走**现有睡觉流程**（`SleepFlow::Request`：睡觉画面 + 3 秒可取消，与按 PWR 一样） |
| 拨轮上/下 | **随时退回普通模式**，这次转动照常生效（下=下一页/出题，上=上一页）；之后不再自动翻页，也不再自动睡 |
| 按下 | 只是“再读一遍”，仍在睡前模式，翻页计时重新开始 |
| 长按 | 回首页 |

边界：退出睡前模式不重画当前页，所以那一页会保持低对比度到下一次翻页；这是可接受的（真机上看效果再定）。

### 1.3 系列故事

- 系列信息在书头（§2.2）：系列 id、第几本、共几本、下一本的内容 id。
- **读完一本提示下一本**：结局画面右下角出现“一本书 + 箭头”（`book_page` 第 14 位），板端打 `PET_BOOK_NEXT content=<id>`。**只有卡上真有下一本才显示**：板端用 `WaveshareStorySd::ContentBookPresent` 检查 `/books/c<内容id>.kpb`（服务端目录的卡上命名；手工拷贝的书想被提示就用这个文件名），没有就不画箭头、不打日志。
- **书架排序**：同一系列排在一起，按阅读顺序；其他按 id。`ShelfBefore`（C++）和 `kpb2.shelf_order`（Python）是同一个键；`python -m tools.story_pack shelf --dir <卡上的 books 目录>` 打印顺序。
- **本批没做的**：板子目前固定打开 `forest_friends`（V4 起如此），没有书目列表和换书；提示的“下一本”也只是画面和日志提示，真正换书要等书架（不在这批范围）。服务端目录用 `/books/c<content_id>.kpb` 命名，系列元数据已经在 `inspect()` 里提供。

## 2. 格式变化：KPB2 版本 3（向后兼容）

### 2.1 兼容规则

- 版本 **2**（所有旧书）：规则一字不改，读取器照旧；没有链接也没有系列的书，编译器**继续输出版本 2、与黄金样例逐字节相同**。
- 版本 **3**：只在用到分支或系列时才写。头部 `flags`、保留区和页/选项里原来“必须为 0”的字节开始有含义。
- **旧固件**（只认版本 2）遇到版本 3 会拒绝并显示“书坏了/占位”，不会误读。新固件读版本 2 和 3。
- 版本 3 里没用到的字段仍必须为 0；版本 2 里这些字节必须为 0（与以前相同）。

### 2.2 新字段

| 位置 | 字段 | 规则 |
|---|---|---|
| 头 `flags` bit0 | 分支书 | 版本 3 才可以置位；置位 ⇒ 至少有一页带显式链接 |
| 头 64..72（4×u16） | 系列 `id, index, count, next_content_id` | 全 0 = 单本。否则 id≠0，1≤count≤16，1≤index≤count；index<count ⇒ next≠0 且≠本书 id；index=count ⇒ next=0 |
| 头 72..92 | 保留 | 全 0 |
| 页记录 64..66（u16） | `next`（只在分支书里） | 0 = 下一页（最后一页之后 = 结局）；`0xFFFF` = 结局；`0x8001` = 回到第 1 页（“再读一遍”）；其他 = 页号。页记录 66..96 必须为 0 |
| 选项记录 byte 3 | `goto`（只在分支书里） | 0 = 不跳；1..32 = 选了它就跳到该页；`0x81` = 回到第 1 页。其他值非法 |

### 2.3 校验器（`kpb2.parse`、`kpb2.load_source`、C++ `ContentPackReader::Open`、服务端 S4 导入都用同一套规则）

- 选路题：**全部选项都有 `goto` 或都没有**；没有对错（`correct = 0`）；只能是本页**最后一道题**；带选路题的页自己不能再写 `next`。
- 跳转目标/`next` 必须是存在的页，不能指向自己；`goto/next` 里的 `restart` 不能出现在第 1 页。
- **每一页都能从第 1 页走到**；**没有环**（回到第 1 页的 `restart` 是唯一允许往回走的边，且不算“走到结局”）；**选路题的第一个选项不能是 restart**（睡前模式沿用第一个选项，必须能走到结局）；**每一页都能不靠 restart 走到结局**。
- 系列字段按 §2.2 校验；违反任何一条整本拒绝。

### 2.4 书源 `book.json`（仍是 `schema_version: 2`，新字段都可选）

```json
{
  "series": {"id": 1, "index": 1, "count": 3, "next_content_id": 1002},
  "pages": [
    {"number": 2, "text": "…", "next": "end",            // 也可 "restart" 或页号
     "questions": [{"prompt": "…", "options": [
        {"key": "garden", "goto": 3, "…": "…"},          // goto: 页号 或 "restart"
        {"key": "river",  "goto": 5, "…": "…"}]}]}
  ]
}
```

`python -m tools.story_pack validate` 现在会打印 `branching=` 和 `series=`；旁路清单多了 `branching`、`series`、每页 `next`、每个选项 `goto`。

## 3. 代码

| 文件 | 变化 |
|---|---|
| `kid_app/content_pack_reader.{h,cc}` | 解析版本 3：系列、页 `next`、选项 `goto`；`StoryGraph` 校验；`Version()/Branching()/Series*()` |
| `kid_app/waveshare_story_sd.{h,cc}` | `ContentBookPresent`：下一本是否真在卡上 |
| `kid_app/book_reader.{h,cc}` | 大纲带链接/系列；历史栈翻页；`Forward/Advance/Retreat/Finish`；睡前模式（自动翻页、不出题、结局困倦脸、`BookStep.sleep/bedtime_off`）；`BedtimeAvailable/BedtimeTone`；`ShelfBefore`；场景字节：bit6 = 下一本提示，bit7 = 睡前 |
| `kid_app/scene_model.cc` | 结局页的“下一本”小图标 |
| `scripts/p1_integrate.py` | 进入故事时按日相决定睡前模式；灰度页低对比度；`step.sleep` → 现有睡觉流程；日志 `PET_BOOK_BEDTIME`/`PET_BOOK_NEXT` |
| `tools/kpb2.py`、`tools/story_pack.py` | 编译/解析/校验、`shelf`、`shelf_order` |
| `services/pet_learning/content.py` | `inspect()` 带出 `branching`/`series`（校验来自同一个 `kpb2.parse`） |
| `tools/make_branch_test_book.py` | 合成的分支测试书（非儿童内容） |
| `tools/tuantuan_series.py`、`curriculum/stories/tuantuan/*.story.json` | 团团系列（草稿）脚本和生成器 |

## 4. 测试

| 文件 | 覆盖 |
|---|---|
| `tests/cpp/test_branch_book.cc`（`scripts/p2_test.sh`） | 版本 3 解析；22 种坏链接/坏系列被拒；restart 合法；分支走两条路、“上一页=来时的页”、选路反馈和跳转、各结局读完写一次、结局后再读；睡前：自动翻页时间、不出题不写喜好、自动读完不写 `BookFinished`（退回普通模式后读完才写）、分支走第一个选项、结局困倦脸、6 秒后只发一次睡觉；拨轮随时退出（含结局页取消睡觉）；按下留在睡前；线性旧书不变；场景编码；低对比度映射；书架排序 |
| `tests/test_v8_books.py` | 版本头/系列/链接字节；旧书（黄金样例）仍是版本 2；18 种篡改包被拒；源码级错误信息；侧车；书架；板端契约；团团三本书可编译、系列链、分支每条路都到结局、草稿不预先 approved、喜好题只用白名单 |
| `tests/test_v6_content_catalog.py` | 服务端 S4：分支书导入为草稿，带系列信息；坏跳转、环、不可达页、坏系列 `PACK_INVALID` |
| 既有 | `test_v4_kpb2.py`、`test_book_reader.cc`、`test_content_pack_reader.cc` 不改断言继续通过（只把“版本 3 非法”的用例改成“版本 4 非法”） |

## 5. 素材清单

**测试用（非儿童内容）**：`branch_test`（内容 901，6 页，合成图形和正弦音）——CI 现场生成，不入库。

**团团系列（草稿，需要成人审核）**，脚本在 `curriculum/stories/tuantuan/`，系列 id 1：

| # | 书（book_id） | 内容 id | 页数 | 类型 |
|---|---|---:|---:|---|
| 1 | 《团团走哪条路》`tuantuan_road` | 1001 | 8 | **分支书**：第 2 页选花园/小河，两条路各有结局（第 7、8 页） |
| 2 | 《团团和小星星》`tuantuan_star` | 1002 | 6 | 直线睡前短篇，无题 |
| 3 | 《团团睡觉啦》`tuantuan_sleep` | 1003 | 6 | 睡前书，第 3 页一道喜好题（最喜欢的玩具：娃娃/小球） |

- 每页文字 ≤ 60 字、短句，3–5 岁；原创；不含吓人、危险模仿或隐私内容。
- **插画**是 `tools/tuantuan_series.py` 用 Pillow 画的简笔草稿（灰度），**需要插画师或家长润色**。
- **朗读**走现有 TTS 流水线（`content_pipeline.speech`，Edge TTS 晓晓，`docs/LEARNING_REAL_ASSETS_TTS.md`），生成的是草稿，须成人逐句试听；`--tts tone` 只是测试用占位音。
- 生成：`python -m tools.tuantuan_series build --output research_content/stories --tts edge`（需要 edge-tts 和 ffmpeg；输出在被 git 忽略的 `research_content/`）。生成的 `book.json` 里**所有 `approved` 都是 `false`**，`reviewer` 写“待成人审核”，旁边有 `DRAFT_STATUS.json`；审核流程见 [V8_STORY_REVIEW.md](V8_STORY_REVIEW.md)。
- CI 的 `pack-draft` 只用占位音和临时的 approved 标志来证明流水线和故事图能编译；产出不是成品。

## 6. 验收清单

自动（CI）：

- [ ] `scripts/p2_test.sh`（含 `test_branch_book`）、`test_v8_books.py`、`test_v4_kpb2.py`、`test_v4_book_reader.py`、`test_p4_story_pack.py`、服务端 `test_v6_content_catalog.py` 全绿；
- [ ] 固件 CI 能编译（`p1_integrate.py` 的改动在 CI 里才会被编译，本机没有 ESP-IDF）。

真机（由主会话统一刷机，**本会话没有烧写**）：

- [ ] 生成并拷贝分支测试书和团团三本草稿（占位音）到 TF 卡，`book` 自测脚本或临时改 `story_book_` 打开；
- [ ] 分支书：第 2 页下转出选路题；选花园/小河后都跳对；上一页回到来时的页；两个结局都出“读完了”，系列书的结局页有“下一本”小图标；
- [ ] **睡前模式画面**：把 RTC 设到晚间（默认 19:30 之后）进入故事——**低对比度效果**是否舒服（深灰/浅灰在 4 灰度波形上的实际明暗，是否太灰、残影）；
- [ ] **自动翻页节奏**：朗读结束后停 2.5 秒是否合适（太快/太慢调 `kBedtimePauseMs`），读完 6 秒困倦脸后进入睡觉流程是否顺；
- [ ] 睡前中途转拨轮：马上变普通模式，这次转动有效，不再自动睡；
- [ ] 结局页“下一本”小图标位置和大小在 800×480 上是否好看、不挡文字；
- [ ] 灰度失败退回 1-bit 时睡前模式是否仍然工作（只是不降对比度）。
