# V4-05：绘本阅读器（灰度页、朗读、翻页、再读一遍、页内选择题）

对应 `DEVELOPMENT_PLAN.md` V4-05，`PRODUCT_REQUIREMENTS.md` §4（一套操作：拨轮上/下 = 换一个，按下 = 就要这个，长按 = 回家）、§5.2（READER）、§7，ADR-012（喜好只来自拨轮选择题）、ADR-013（绘本页 4 灰度全刷，选择高亮 1-bit 局刷）。数据格式见 [V4_KPB2_CONTENT_FORMAT.md](V4_KPB2_CONTENT_FORMAT.md)。

## 1. 孩子怎么用

| 场景 | 拨轮下 | 拨轮上 | 按下 | 长按 |
|---|---|---|---|---|
| 书页 | 下一页（最后一页之后是“读完了”画面） | 上一页 | 再读一遍（本页朗读从头开始） | 回首页 |
| 出题中 | 高亮移到下一个图标（报选项名） | 高亮移到上一个图标 | 回答 | 回首页 |
| 答题反馈（3 秒） | 不响应 | 不响应 | 不响应 | 回首页 |
| 读完了 | — | 回到最后一页 | 从第 1 页再读 | 回首页 |

- 翻到一页：先全刷画面，**画面上屏后**才开始朗读（与 P4 规则相同）。
- 有题的页：朗读结束后 1.5 s 出第 1 题：在本页画面上叠加 2–3 个图标卡片（白底，第一个带高亮框），播放题干录音。
- 回答：喜好题/无对错题标出孩子选的那个；知识题答对标出所选，**答错则标出正确的那个并念它的名字**（不放“错了”的声音）。3 s 后出下一题，或回到不带卡片的本页（不再朗读）。
- 出题前翻页就跳过本页的题；再翻回来是新的一次阅读，题目会再出。
- 不支持语音回答（`voice_answer` 位现在被忽略）；BOOT 键在阅读器里的旧行为（单击 = 持久化 NextPage）没有改动，但它不再改变画面上的页。

### 没有书的时候

TF 卡没插、卡里没有这本书、或书文件打不开时，进入故事不再是一本翻不动的 0 页书，而是一屏只靠图标的提示（不识字也看得懂）：

| 原因 | 画面 | 日志 |
|---|---|---|
| 没插卡（`no_card`） | 团团头上一个“？”，右边一张存储卡 + 向下箭头指向卡槽，下方一个“小房子”卡片 | `PET_BOOK_MISSING reason=no_card` |
| 卡里没有书（`no_book`） | 同样的团团，右边一本翻开的空白书 + 三个点，下方“小房子”卡片 | `PET_BOOK_MISSING reason=no_book` |
| 书文件坏了（`bad_book`，格式校验失败） | 与 `no_book` 同一幅画 | `PET_BOOK_MISSING reason=bad_book` |

- 拨轮 **按下** 或 **长按** 都回首页（`PET_BOOK_HOME`，走与回家相同的持久化 `StopStory`）；上/下转动什么也不做。
- 不朗读、不出题，不产生任何阅读记忆/喜好（`BookStep.memory` 恒为空，也没有 `BookFinished`）。可选的语音短句本批未加，需要时由 `PlayBookClip` 同路径加一个提示音。
- 逻辑在 `kid_app`：`BookMissing`（`scene_model.h`，编码在 Reader 场景的 `book_page`：低 8 位 = 0，高 8 位 = 原因，页码从 1 开始所以不会冲突）、`BookReader::Open(outline, now, missing)` 的 `Phase::Missing`、`SceneComposer` 的 `BookMissingPicture`。画面是普通场景（`ShowPetScene` 全刷），不读卡。
- 原因来自 `WaveshareStorySd::Missing()`：挂载失败 = `NoCard`；挂载成功但 `SD_BOOK_ABSENT` = `NoBook`；包校验失败 = `BadBook`（板端另在摘要生成失败时置 `BadBook`）。该文件仍不出现 `fopen(`。
- 板端启动时若上次停在故事里而没有书，原有的 `PET_STORY_RESTORE_HOME reason=no_book` 直接回首页，不变。

## 2. 结构

```
pet_input 任务                       kid_epd 任务
  BookReader（纯 C++，已主机测试）        DrawBookLocked
  拨轮 → BookStep                         新页：4 灰度全刷 / 1-bit 全刷
  ├ publish：Reader 场景 ──邮箱──────►     出题/高亮：1-bit 局刷
  ├ clip：story_pcm_.RequestClip          ↓ 上屏成功
  ├ memory：RecordBookMemory           book_shown_ ──► BookReader::OnShown → 开始朗读/题干
  └ home：PostLocal(StopStory)
```

- `kid_app/book_reader.h`：`BookOutline`（打开书时一次性从卡上读出的页/题摘要，此后输入任务不再读卡）、`BookReader` 状态机、`BookScene/BookViewOf`（Reader 场景的 `book_page` 低 8 位 = 页，高 8 位 = 题目叠加码）、`BookMemoryEvent`（喜好守卫）、`ComposeBookOverlay/ForceBookOverlay`（叠加帧）、`EpdGreyPlanes`（4 灰 RAM 平面）。
- `kid_app/icon_choice.h`：可复用的“N 个图标选一个”（拨轮移动、夹在两端、高亮/选中环、强制局刷旧图），游戏可直接用。
- `FreertosStoryPcmWorker::RequestClip`：同一个朗读任务现在也能播题干和选项名（KPB2 规定都是 24 kHz 单声道 PCM16），最新请求优先，`RequestPage` 不变。
- 翻页不再是持久化动作：页码像首页动作条的高亮一样只在 UI 里（关机后从第 1 页开始）。只有进入（StartStory）和回家（StopStory）写 NVS。

## 3. 屏幕刷新

| 时机 | 刷新 | 数据 |
|---|---|---|
| 新的一页、题目结束后恢复本页 | 全刷：有灰度帧 → 4 灰度；否则 1-bit | 卡上灰度帧 / 1-bit 帧 |
| 出题（第一帧） | 1-bit 局刷，**旧图 = 面板现状，但卡片区域取新图的反色**：卡片区每个像素都被驱动（底下可能是灰色），上一题的卡片也被擦掉 | 1-bit 帧 + 图标 + 环 |
| 移动高亮、标出答案 | 1-bit 局刷，旧图 = 上一帧，只改动环的像素 | 同上 |
| 读完了 | 阅读器占位画面（全刷，团团开心） | — |

### 4 灰度波形（`ShowBookGrey`）

按微雪官方 3.97 示例 `EPD_3IN97_Init_4GRAY` / `EPD_3IN97_Display_4Gray`（Arduino 示例 `02_E-Paper_Example`）：

1. `EPD_Init()`（与官方 4 灰初始化相同的寄存器），`0x1A = 0x5A`（固定温度值，选中面板 OTP 里的 4 灰波形）；
2. RAM `0x24` 写 `NOT lo`、RAM `0x26` 写 `NOT hi`（KPB2 灰度级 0 黑 … 3 白，`hi = L>>1`，`lo = L&1`；即官方示例里黑和浅灰在 0x24 为 1，黑和深灰在 0x26 为 1）；
3. `0x22 = 0xD7`、`0x20`、等 BUSY（不重新读温度传感器）；
4. 立刻再 `EPD_Init()`，把两块 RAM 都写成本页的 1-bit 回退帧（不刷新）：之后的局刷/全刷和以前一样从传感器取温度、和 1-bit 帧比较。

灰度级和平面的对应、深灰/浅灰的明暗、残影都**只能在真板上确认**（本 PR 没有上板）。如果真板显示不对：`EpdGreyPlanes` 是唯一的映射处（主机测试锁定了官方示例的映射）；若 4 灰路径整体不可用，把板端 `kBookGreyEnabled` 改为 `false`，所有页改用 1-bit 回退帧，格式和阅读器都不用改。灰度刷新失败时自动退回 1-bit 全刷（日志 `how=mono_after_grey`）。

## 4. 喜好与记忆（ADR-012）

- 只有 **出题中按下拨轮** 才会产生喜好请求，且只针对包里标了 like 的选项（“都喜欢”这类不算）；知识题、移动高亮、长按、语音都不会。
- 请求只经 `BookMemoryEvent` → `ChoiceMemoryEvent`（来源 `DialChoice`）变成 `PREFERENCE_CHOSEN`；读完 KPB2 书（每次打开一次）生成 `BookFinished{content_id}`。板端代码里没有自己拼喜好事件的地方（契约测试检查）。
- 板端 `RecordBookMemory` 把事件 id 和日期置 0 后交给 `RecordMemory`（PR #64 的原子账本入口，Pet 任务上分配本地 id、取 RTC 日期，与宠物状态同一事务落盘），日志 `PET_BOOK_MEMORY kind=like like=<slot>:<option> posted=1`，账本提交结果见 `PET_MEMORY_COMMIT`。

## 5. 测试

| 文件 | 覆盖 |
|---|---|
| `tests/cpp/test_book_reader.cc`（`scripts/p2_test.sh`） | 黄金 KPB2/KPB1 的摘要；场景编码往返；整本流程（朗读只在上屏后、出题时机、高亮夹边、回答/反馈/下一题/恢复、再读一遍、跳题和重访、读完一次、长按回家、无书）；喜好守卫（只有 like 选项、知识题/“都喜欢”/越界/无 id 拒绝，Voice 来源被 MemoryStore 拒绝）；叠加帧像素、强制旧图（含题与题之间）；4 灰平面与官方映射；IconChoice；工具生成的测试书完整走一遍 |
| `tests/test_v4_book_reader.py` | 测试书工具可复现、能被严格解析；板端契约（官方 4 灰顺序、局刷先写旧图、拨轮路由在睡觉/家长菜单之后、只有 StopStory 持久化、卡读取不在输入任务、喜好只走守卫、自测 `book` 只在首次进入 Pet 前生效）；自测场景 |
| `tests/cpp/test_selftest_script.cc` | 新增 `book <id>` |
| 现有契约测试 | 朗读仍只在上屏后开始、不在入睡时开始；无卡/越界/读完时回退占位画面 |

## 6. 真板自测（只由协调会话执行）

1. 生成测试书（非儿童内容）：`python -m tools.make_reader_test_book --output build/reader-test`（CI 的 p4-story-pack 也会上传 artifact `waveshare-tf-reader-test-only`）。
2. 把 `build/reader-test/books/reader_test.kpb` 复制到 TF 卡 `/books/reader_test.kpb`，**保留** `/books/forest_friends.kpb`。设备平时仍读 `forest_friends`；只有自测脚本里的 `book reader_test`（必须在第一个 `pet` 之前）才改读测试书。
3. 场景 `tools/selftest/v4_book_reader.pettest`（约 2.5 分钟）：打开书 → 第 1 页灰度 → 出题 → 移动高亮 → 选蓝色（喜好 2:3）→ 第 2 题答错 → 恢复本页 → 再读一遍 → 第 2、3 页来回 → 读完 → 回到第 3 页 → 回家。重点看：灰度是否正确（4 条横带由黑到白）、卡片局刷是否干净、灰度残影、`PET_DIAG_EPD` 的全刷耗时（含读卡）。
4. `tools/selftest/v2_home_rail.pettest` 的 `commits` 期望从 4 改为 3（阅读器翻页不再提交）。
5. 无书场景 `tools/selftest/v4_book_missing.pettest`（约 1.5 分钟，监视 ≈ 100 s）：`book no_such_book`（卡上不存在的 id，必须在第一个 `pet` 之前）→ 打开故事看到“没有书”画面 → 拨轮转动无反应 → 按下回家 → 再进一次 → 长按回家。期望 `PET_BOOK_MISSING` ×2、`reason=no_book`、`PET_BOOK_HOME` ×2、无 `PET_BOOK_AUDIO`/`PET_BOOK_MEMORY`、提交数 4。需要卡已插好；没卡时原因是 `no_card`，改期望即可。`v4_book_reader` 的期望不变。
