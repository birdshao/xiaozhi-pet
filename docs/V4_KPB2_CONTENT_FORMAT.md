# V4-04 / V4-09：KPB2 绘本内容包格式与制作工具

对应 `DEVELOPMENT_PLAN.md` V4-04、V4-09（为 V4-05 绘本阅读器准备数据），`PRODUCT_REQUIREMENTS.md` §5.2、§7，ADR-012（喜好只来自拨轮选择题）、ADR-013（绘本页用 4 灰度全刷）。KPB1 的格式见 [P4_OFFLINE_STORY_PACK.md](P4_OFFLINE_STORY_PACK.md)，本格式**不替换**它：设备同时读 KPB1 和 KPB2。

家长制作和审核步骤见 [V4_CONTENT_REVIEW_CHECKLIST.md](V4_CONTENT_REVIEW_CHECKLIST.md)。

## 1. 一句话

一个 `.kpb` 文件 = 一本书。KPB2 在 KPB1 的基础上增加：

- 每页可带一张 **4 级灰度** 插图（2 bit/像素），同时永远带一张 **1-bit 回退帧**；
- 每页 **0–2 个选择题**：题干录音 + 2–3 个选项，每个选项有一个 1-bit 图标、它在页面上的位置（= 拨轮高亮区域）、选项名录音和文字；
- 题目可以有“正确答案”（知识题），或者没有对错、作为**喜好题**写入记忆（只限白名单里的主题和选项）；
- 包头带 **包类型**（现在只有绘本）、数字内容 id、修订号、书名。

## 2. 魔数、版本、包类型、兼容

| 项 | 值 |
|---|---|
| 魔数 | `KPB2`（KPB1 文件仍是 `KPB1`） |
| 版本 | uint16 `2` |
| 包类型 `package_type` | `1` 绘本 book（本 PR 唯一可编译、可读取的类型）；`2` 声音包 soundpack（**保留**给声音包工作）；`3` 游戏 game、`4` 装扮 costume、`5` 话术 phrases（保留）；`0` 与其他值非法 |
| 字节序 | 全部小端 |

- 设备读取器看到 `KPB1` 走原规则（与 P4 发布时逐字段一致，已在卡上的 KPB1 照常打开），看到 `KPB2` 走本文规则，其他魔数一律拒绝。
- **旧固件**只认 `KPB1`，插入 KPB2 卡会安全地显示占位页，不会误读。
- 包类型不是 1 的 KPB2 文件，本读取器拒绝打开——声音包、游戏包永远不会被当成书显示。别的包可以用自己的魔数和格式；若要共用 KPB2 外壳，必须用上面保留的类型值。
- 以后若要改变已有字段含义，必须换版本号；只在保留区里加字段且旧读取器能安全拒绝时，也要换版本号（读取器要求保留区全 0）。

## 3. 文件总体布局

```
[0      ] 头部            96 B
[96     ] 页表            页数 × 96 B
[ .. ]    题目表          题目总数 × 192 B
[data   ] 数据区          书名、各页画面/音频/文字、各题题干/选项素材（规范顺序，4 字节对齐）
```

页表和题目表紧挨着，用一个 CRC32 整体保护；头部有自己的 CRC32；数据区的**每一段**都有自己的 CRC32（记在表里）。

## 4. 头部（96 字节，`struct "<4s10H10I28sI"`）

| 偏移 | 类型 | 字段 | 规则 |
|---:|---|---|---|
| 0 | 4s | magic | `KPB2` |
| 4 | u16 | version | 2 |
| 6 | u16 | header_bytes | 96 |
| 8 | u16 | package_type | 1（见 §2） |
| 10 | u16 | flags | 0（保留） |
| 12/14 | u16 | width / height | 800 / 480 |
| 16 | u16 | content_id | 1–65535，书的数字 id；读完一本书时写入记忆（`BookFinished.option`） |
| 18 | u16 | revision | 1–65535，同一本书的修订号 |
| 20 | u16 | page_count | 1–32 |
| 22 | u16 | question_count | 0–64（全书题目总数） |
| 24/28 | u32 | page_table_offset / bytes | 96 / 页数×96 |
| 32/36 | u32 | question_table_offset / bytes | 紧接页表 / 题数×192 |
| 40 | u32 | data_offset | 紧接题目表 |
| 44 | u32 | total_bytes | 等于文件大小，≤ 64 MiB |
| 48/52/56 | u32×3 | title offset / bytes / crc | 书名 UTF-8，1–320 B，是数据区第一段 |
| 60 | u32 | tables_crc | `[96, data_offset)`（页表+题目表）的 CRC32 |
| 64 | 28 B | reserved | 全 0 |
| 92 | u32 | header_crc | 前 92 字节的 CRC32 |

## 5. 页记录（96 字节，`struct "<4H3I3I3IIHH3I32s"`）

| 偏移 | 类型 | 字段 | 规则 |
|---:|---|---|---|
| 0 | u16 | number | 第 i 条必须是 i（从 1 开始） |
| 2 | u16 | flags | bit0 = 有灰度页；其他位 0 |
| 4 | u16 | question_first | 本页第一题在题目表里的下标（= 前面各页题数之和） |
| 6 | u16 | question_count | 0–2 |
| 8 | 3×u32 | mono offset/bytes/crc | 1-bit 页，恰好 48,000 B |
| 20 | 3×u32 | grey offset/bytes/crc | 有灰度页时恰好 96,000 B；没有时三个字段全 0 |
| 32 | 3×u32 | pcm offset/bytes/crc | 朗读，2 B–2,880,000 B（≤60 s），偶数 |
| 44 | u32 | sample_rate | 24000 |
| 48/50 | u16 | channels / bits | 1 / 16 |
| 52 | 3×u32 | text offset/bytes/crc | 审核过的页面文字 UTF-8，1–1,280 B |
| 64 | 32 B | reserved | 全 0 |

## 6. 题目记录（192 字节）

前 32 字节 `struct "<HBBBBH3I3I"`，后接 3 个 48 字节选项槽，再接 16 字节保留（全 0）。

| 偏移 | 类型 | 字段 | 规则 |
|---:|---|---|---|
| 0 | u16 | page | 所属页号 |
| 2 | u8 | index | 本页第几题，0 或 1 |
| 3 | u8 | option_count | 2 或 3 |
| 4 | u8 | correct | 0 = 没有对错；1–3 = 正确选项（从 1 数） |
| 5 | u8 | flags | bit0 = 允许语音回答；其他位 0 |
| 6 | u16 | like_slot | 0 = 本题**永不写记忆**；非 0 = 喜好主题 id（§10） |
| 8 | 3×u32 | prompt pcm | 题干录音，≤15 s，偶数字节 |
| 20 | 3×u32 | prompt text | 题干文字，1–400 B |
| 32 | 3×48 B | options[3] | 未用的槽必须全 0 |
| 176 | 16 B | reserved | 全 0 |

选项（48 字节，`struct "<HBB4H3I3I3I"`）：

| 偏移 | 类型 | 字段 | 规则 |
|---:|---|---|---|
| 0 | u16 | id | 喜好选项 = 白名单选项 id（1–0xFEFF）；其他选项 = `0xFF00 + 位置`（0xFF01..0xFF03） |
| 2 | u8 | flags | bit0 = 选它算“喜欢”（写喜好记忆）；其他位 0 |
| 3 | u8 | reserved | 0 |
| 4/6/8/10 | u16×4 | x / y / w / h | 图标在页面上的位置和大小 = 拨轮高亮区域 |
| 12 | 3×u32 | icon | 1-bit 图标，恰好 w×h/8 字节，行优先、MSB 在左、bit=1 白 |
| 24 | 3×u32 | pcm | 选项名录音，≤5 s，偶数字节 |
| 36 | 3×u32 | label | 选项名文字，1–96 B |

几何规则（编译器和读取器都检查）：

- `w`、`h` 在 48–240 px；`w` 和 `x` 都是 8 的倍数（1-bit 帧按字节对齐，局刷窗口 X 方向按 8 像素寻址）；
- 图标四周留 8 px 高亮边（`HighlightRect` = 图标框各向外扩 8 px），整个高亮框必须在 800×480 内；
- 同一题的各选项高亮框互不重叠；不同题之间可以重叠（一次只显示一题）。

语义规则：

- `like_slot ≠ 0`（喜好题）⇒ `correct = 0`，且至少一个选项 `flags.bit0 = 1`；
- `flags.bit0 = 1` 的选项 ⇒ 本题 `like_slot ≠ 0`，id 来自白名单，同题内不重复；
- `like_slot = 0` ⇒ 所有选项都不算喜欢，id 必须是 `0xFF00 + 位置`。

## 7. 数据区规范顺序

数据区里各段**按固定顺序**依次排列，每段起点 = 上一段终点向上取整到 4 的倍数，中间的填充字节必须为 0，最后一段结束处就是文件末尾：

```
书名
第 1 页：mono、grey（若有）、pcm、text
  第 1 页第 1 题：题干 pcm、题干 text、选项1 icon/pcm/label、选项2 …、选项3 …
  第 1 页第 2 题：…
第 2 页：…
```

读取器沿着这个顺序逐段检查“偏移恰好在这里 + 填充为 0 + CRC 正确”，所以段与段不可能重叠、不可能藏数据、不可能有尾巴。

## 8. 画面

### 8.1 编码

| 帧 | 大小 | 编码 |
|---|---|---|
| 1-bit（mono） | 48,000 B | 与 `MonoFrame`/KPB1 相同：每行 100 B，MSB 在左，bit=1 白 |
| 4 灰度（grey） | 96,000 B | 每行 200 B，每字节 4 个像素，MSB 在左：`byte = p0<<6 | p1<<4 | p2<<2 | p3`；0 = 黑、1 = 深灰、2 = 浅灰、3 = 白 |
| 选项图标 | w×h/8 B | 同 1-bit 帧编码，行宽 w/8 |

编译器的转换规则（确定性，同输入同输出）：

- 灰度级 = `(亮度 × 3 + 127) // 255`，即 0/85/170/255 最近邻；
- 灰度页的 **1-bit 回退帧** = 2×2 有序抖动：灰度级 L 在 `L > [[0,1],[1,2]][y%2][x%2]` 处为白（深灰 25 % 白点、浅灰 75 % 白点，纯黑纯白不变）；
- `image_mode: "mono"` 的页和选项图标：亮度 ≥170 为白（与 KPB1 相同阈值），没有灰度帧；
- PNG 的透明像素按白色处理。

### 8.2 设备怎样显示（V4-05 用）

- **翻到一页**：有灰度帧就用 4 灰度全刷（ADR-013）；没有灰度帧、或灰度刷新不可用时，用 1-bit 回退帧。现有板端 Reader（`StoryPackReader::ReadFrame`）显示的就是 1-bit 回退帧，所以 KPB2 卡插到现在的固件上也能看。
- **4 灰度刷新怎样消费数据**：4 灰波形需要两个 1-bit 平面。`SplitGreyPlanes()` 把灰度字节拆成 `hi = level>>1`、`lo = level&1` 两个平面（各 48,000 B，与 1-bit 帧同布局）。可以一次读 96,000 B 到 PSRAM 再拆，也可以用 `ReadGreyRows()` 每次读几行（每行 200 B）边拆边经 SPI 送出，只需要几百字节缓冲。**哪个平面写进控制器的哪块 RAM、是否需要取反，必须由 V4-05 在 3.97 真板上对照官方 4 灰示例确认**；本格式只固定“0 黑 3 白”和两个平面的公式。
- **出题**：先在当前页上叠加该题的图标（`BlitOptionIcon` 画进 1-bit 帧），对图标所在区域做 1-bit 局刷；拨轮移动时只局刷前后两个选项的 `HighlightRect`（在 8 px 边里画/擦高亮框）。图标和高亮都是纯黑白，所以 1-bit 局刷不会把灰色画面弄脏；题目结束后用该页画面（灰度全刷或 1-bit 回退帧对应区域）恢复。

## 9. 音频

页朗读、题干、选项名都是 24 kHz、单声道、PCM16 小端，无 WAV 头，与 KPB1 和现有 `FreertosStoryPcmWorker` / `AudioService` 本地 PCM 队列一致。长度上限：页 60 s、题干 15 s、选项名 5 s。读取接口按偶数字节偏移分块读取，不需要整段缓冲。

## 10. 题目、语音回答和记忆（ADR-012）

- **知识题**（`correct ≠ 0`）：答对/答错只用于即时反馈（夸一夸、再听一遍），**不写任何记忆**。
- **喜好题**（`like_slot ≠ 0`）：孩子用**拨轮选中并按下确认**一个 `flags.bit0 = 1` 的选项时，设备用 `ChoiceMemoryEvent()` 生成 `PREFERENCE_CHOSEN{slot = like_slot, option = 选项 id, source = DialChoice}`，交给账本 `RecordMemory`。这是绘本写喜好的**唯一**入口。选了不算喜欢的选项（例如“都喜欢”）不写记忆。
- **语音回答**（`flags.bit0 = 1` 的题）：孩子说出选项名时，设备只能拿它和该题选项文字做本地匹配、给出同样的即时反馈，或者把高亮移到那个选项等待拨轮确认；**语音结果永远不调用 `ChoiceMemoryEvent`**，不写喜好（`MemoryStore` 本来也会拒绝 `Voice` 来源）。
- **读完一本书**：`BookFinishedMemoryEvent()` 生成 `BookFinished{option = content_id, source = Device}`。KPB1 没有数字 id，不生成。

### 喜好主题白名单（`tools/kpb2.py` `PREFERENCE_TOPICS`）

id 一经发布**不得改号、不得复用**，只能在末尾追加；设备记忆里只存数字，家长端按这张表显示名字。

| like_topic | slot | 选项 key = id |
|---|---:|---|
| favorite_animal 最喜欢的动物 | 1 | cat 1 小猫、dog 2 小狗、rabbit 3 小兔子、bird 4 小鸟、fish 5 小鱼、elephant 6 大象、panda 7 熊猫、bear 8 小熊、duck 9 小鸭子、turtle 10 小乌龟 |
| favorite_color 最喜欢的颜色 | 2 | red 1、yellow 2、blue 3、green 4、pink 5、purple 6、orange 7、white 8、black 9 |
| favorite_food 最喜欢的食物 | 3 | carrot 1、apple 2、banana 3、strawberry 4、rice 5、noodles 6、egg 7、milk 8 |
| favorite_toy 最喜欢的玩具 | 4 | ball 1、blocks 2、doll 3、car 4、kite 5、drum 6 |

同一个东西在不同的书里是同一个 id（例如任何书里的“小兔子”都是 slot 1 / option 3），所以跨书的喜好能对上。

## 11. 上限一览

| 项 | 上限 |
|---|---|
| 包大小 | 64 MiB |
| 页数 | 1–32 |
| 每页题数 / 全书题数 | 0–2 / 64 |
| 每题选项 | 2–3 |
| 书名 / 页文字 / 题干 / 选项名 | 80 / 320 / 100 / 24 个字（字节上限 320 / 1,280 / 400 / 96） |
| 页朗读 / 题干 / 选项名录音 | 60 / 15 / 5 s |
| 图标 | 48–240 px，宽为 8 的倍数 |

## 12. 完整性

- 头部 CRC、表 CRC、每段 CRC32（与 KPB1 同一多项式）。读取器在 `Open()` 时用 512 B 栈缓冲把**每一段**都校验一遍，全部通过才报告可用；KPB1 也是这样做的。整本书一次性校验的耗时随书变大，需要在真板上实测（V4-05）。
- CRC 只能发现意外损坏（SD 坏块、拷贝中断），**不是签名**，不能证明内容经过审核或没有被改过。`approved`、`reviewer` 是家长的声明，只写在源清单和旁路清单里。面向不可信来源（网络同步、别人给的卡）时必须另加签名校验和家长白名单。
- 编译器在写文件前用同一套规则把刚生成的包再严格解析一遍（`kpb2.parse`），解析不过就不写；写入是“临时文件 + 原子替换”。

## 13. 源目录与 `book.json`（schema_version 2）

```
my_book/
  book.json
  page_01.png        800×480，灰度插画
  page_01.wav        24 kHz 单声道 PCM16
  q_animal.wav       题干录音
  icon_rabbit.png    选项图标（48–240 px，宽为 8 的倍数）
  name_rabbit.wav    选项名录音
  ...
```

```json
{
  "schema_version": 2,
  "package_type": "book",
  "book_id": "forest_friends",
  "content_id": 101,
  "revision": 1,
  "title": "森林里的小伙伴",
  "reviewer": "妈妈",
  "approved": true,
  "pages": [
    {
      "number": 1,
      "text": "早晨，小兔子来到森林里。",
      "approved": true,
      "image": "page_01.png",
      "image_mode": "grey",
      "audio": "page_01.wav",
      "questions": [
        {
          "prompt": "你最喜欢哪个小动物？",
          "prompt_audio": "q_animal.wav",
          "approved": true,
          "like_topic": "favorite_animal",
          "options": [
            {"key": "rabbit", "label": "小兔子", "icon": "icon_rabbit.png",
             "audio": "name_rabbit.wav", "x": 96, "y": 320, "like": true},
            {"key": "bird", "label": "小鸟", "icon": "icon_bird.png",
             "audio": "name_bird.wav", "x": 336, "y": 320, "like": true}
          ]
        },
        {
          "prompt": "哪个是圆形？",
          "prompt_audio": "q_shape.wav",
          "approved": true,
          "answer": "circle",
          "voice_answer": true,
          "options": [
            {"key": "circle", "label": "圆形", "icon": "icon_circle.png",
             "audio": "name_circle.wav", "x": 160, "y": 200},
            {"key": "square", "label": "方形", "icon": "icon_square.png",
             "audio": "name_square.wav", "x": 480, "y": 200}
          ]
        }
      ]
    }
  ]
}
```

- 字段集合是严格的：多一个、少一个必填字段都拒绝。可选字段：页的 `image_mode`（默认 `grey`）、`questions`；题的 `answer`、`like_topic`、`voice_answer`（默认 false）；选项的 `like`（默认 false）。
- `answer` 和 `like_topic` 不能同时出现。`like: true` 的选项 key 必须在该主题的白名单里。
- 每页、每题都要 `"approved": true`，整本书也要；编译时还必须加 `--reviewed`。
- `reviewer` 只进旁路清单 `*.manifest.json`，不进 `.kpb`，设备上没有家长身份信息。
- 图片可以是 PNG（需要 Pillow）或二进制 PGM（P5，8 bit，无需额外依赖）。所有路径必须是书目录内的相对路径，不允许 `..`、绝对路径或指向目录外的符号链接。

## 14. 工具命令

```
python -m tools.story_pack pack     --source my_book/book.json --output out/forest_friends.kpb --reviewed
python -m tools.story_pack validate --package out/forest_friends.kpb
python -m tools.story_pack preview  --package out/forest_friends.kpb --output out/preview
```

- `pack`：`schema_version: 1` 照旧编译 KPB1，`schema_version: 2` 编译 KPB2；同时写出 `forest_friends.manifest.json`（每页/每题/每个选项的内容、哪些选项会写喜好、各段 SHA-256、整包 SHA-256、审核人）。
- `validate`：用与设备相同的规则严格检查一个 KPB1/KPB2 文件，打印 `PACKAGE_VALID ...` 或以非 0 退出并打印 `PACKAGE_INVALID 原因`。
- `preview`：不需要 Pillow，写出设备会显示的画面：`pageNN_grey.png`（4 级灰度）、`pageNN_mono.png`（1-bit 回退）、`pageNN_qK.png`（叠加了该题图标和高亮框的画面）。家长审核时看这些图，而不是看原始插画。

## 15. 设备端接口（`kid_app/content_pack_reader.h`）

纯 C++17，无堆分配，对象约 2 KiB（题目记录按需从卡上重读）：

- `ContentPackReader::Open(StoryPackFile&)`：KPB1/KPB2 自动识别，全量校验；`Format()`、`ContentId()`、`Revision()`、`PageCount()`、`QuestionCount()`、`ReadTitle()`。
- 页：`Page()`（`has_grey`、题数）、`ReadMono()`、`ReadGrey()`、`ReadGreyRows()`、`ReadPagePcm()`、`ReadPageText()`。
- 题：`Question(page, index)`、`ReadPromptPcm/Text()`、`ReadOptionPcm/Label/Icon()`、`BlitOptionIcon()`、`HighlightRect()`、`ContentQuestion::IsCorrect()`。
- 刷新：`SplitGreyPlanes()`。记忆：`ChoiceMemoryEvent()`、`BookFinishedMemoryEvent()`。
- `StoryPackReader`（板端 Reader 和 PCM worker 在用的类）接口不变，内部改为委托 `ContentPackReader`，并多了 `Content()` 供 V4-05 拿到同一个已校验的包。`scripts/p1_integrate.py` **没有改动**。

## 16. 测试

| 文件 | 覆盖 |
|---|---|
| `tests/test_v4_kpb2.py` | 黄金样例逐字节可复现；头部/灰度编码/抖动/题目字段；20 种源清单错误（未审核、保留包类型、未知主题、非白名单选项、知识题带喜好、重叠、未对齐、出界、超题数、错采样率、超时长……）；篡改包（含重新封好 CRC 后的语义错误）被拒；KPB1 照常编译和校验；`preview`/`validate` 命令；CI 中用 g++ 编译 C++ 读取器读取工具刚生成的包 |
| `tests/cpp/test_content_pack_reader.cc` | 读取 `tests/fixtures/kpb2` 的黄金 KPB2/KPB1：灰度/回退帧像素、行读取、平面拆分、题目与选项、图标绘制、喜好→`MemoryStore` 落地、知识题/“都喜欢”/语音不写记忆、`StoryPackReader` 适配、20 种损坏与语义错误拒绝；由 `scripts/p2_test.sh` 运行 |
| `tests/test_p4_story_pack.py`、`tests/cpp/test_story_pack_reader.cc` | 原有 KPB1 测试不改断言，继续通过 |

黄金样例 `tests/fixtures/kpb2/golden_book.kpb`（2 页，第 1 页灰度 + 2 题）和 `golden_kpb1.kpb` 都是合成的测试内容，**不是给孩子的读物**。格式有意变更时用 `python tests/test_v4_kpb2.py --write-golden` 重新生成。

## 17. 本 PR 没有做的

- V4-05 绘本阅读器：灰度全刷驱动（扩展 `CustomEpdDisplay`）、出题/高亮局刷、题目音频播放、语音回答匹配、读完贴纸，都没有接到板上。按现有代码路径，板子打开 KPB2 书只会显示 1-bit 回退帧并播放页朗读（KPB2 卡尚未上板验证）。
- 4 灰度平面与控制器 RAM 的对应关系、刷新耗时、灰度残影：待真板确认。
- 声音包、游戏包等其他包类型：只保留了类型值，没有格式和读取器。
- 板端仍固定打开 `forest_friends`，没有书目列表和家长白名单。
- 打开一本大书时全量 CRC 的耗时未在真板上测量。
