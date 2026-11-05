# V4-06 / V4-07 / V4-08：规则小游戏（离线）、KPG1 游戏包与体感游戏

对应 `DEVELOPMENT_PLAN.md` V4-06（知识小游戏）和 V4-07（体感游戏，见 §13），`PRODUCT_REQUIREMENTS.md` §4（拨轮上下换、按下选、长按回家；不用双击/三击）、§5.2（GAME 场景局刷）、§7（题目来自审核内容包，不由模型即兴出题），ADR-011/012/013。

## 1. 一句话

首页动作条选「游戏」按下，先出**游戏菜单**（V4-07，§13：知识题 / 倾斜滚球 / 扣下躲猫猫 / 吹蜡烛 / 拍手节奏，拨轮选、按下开始）；选第一张「知识题」后团团出 3 道题（每题 2–3 张答案卡），孩子拨轮挑一张、按下作答；答对团团开心（唯一的奖励），答错温和地说“再想想”，第二次还不对就把答案指给孩子看然后进入下一题；3 题后回首页。题目来自 TF 卡上家长审核过的 **KPG1 游戏包** `/games/default.kpg`；没有卡或包无效时用固件里的**内置题**（不需要录音）。

## 2. 孩子怎么玩

| 操作 | 效果 |
|---|---|
| 拨轮上 / 下 | 高亮换一张答案卡（到头不再动），播放这张卡的名字 |
| 拨轮按下 | 作答。对：团团笑眯眯 + 爱心，2.5 s 后下一题；错：团团歪头 + 问号，再选；第二次错：团团眨眼，高亮移到正确答案上，3.5 s 后下一题（不算答对） |
| 拨轮长按 | 马上回首页 |
| 12 s 没操作 | 再说一遍题目（每题一次） |
| 60 s 没操作 | 回首页 |
| BOOT 按一下 | 和团团说话：游戏结束，进入语音 |
| PWR 按一下 | 哄睡：游戏结束，走原来的睡觉流程 |

- 团团做反应（对 / 揭晓）的那几秒里，按键都不起作用，所以连按不会多拿奖励。
- 每局的题目顺序和答案卡位置由随机种子决定（同种子同结果，`GameSession` 是确定性的）；正确答案不会总在同一个位置。
- 拨轮高亮复用绘本页内选择题（V4-05）的 `IconChoice`（同样在两端夹紧）；画面不复用绘本的 `DrawChoiceRing`，因为游戏的答案卡是固件图库画的固定卡片，不是叠在灰度页上的图标。
- 不用双击、三击；游戏中 BOOT 的调试多击被忽略。

## 3. 游戏种类与内置题

| kind | id | 说明 | 内置 |
|---|---:|---|---|
| `match_shape` 找一样的形状 | 1 | 题目卡显示一个形状，选相同的 | 4 题（圆、星、三角、心） |
| `count` 数一数 | 2 | 题目卡显示 N 块积木，选 N 个圆点 | 4 题（1–4） |
| `animal_sound` 听声音猜动物 | 3 | 题目卡是喇叭，播放叫声录音 | 无（需要录音，只能来自游戏包） |
| `big_small` 大和小 | 4 | 题目录音问“哪个大/小” | 无 |
| `english_word` 英语单词卡 | 5 | 题目录音说英文单词 | 无 |

内置题是“看图配对”，不用识字也不用听懂话：题目卡就是要找的东西。内置题的声音是固件现场合成的提示音（题目音、每张卡一段短旋律；数数卡响 N 声），所以不需要家长录音。

**颜色**：墨水屏只有黑白，没法出“找红色”这类题；PRD 里的颜色游戏不在本 PR 里（可以以后用 4 灰度做“黑/灰/白”）。

## 4. 奖励与记忆（ADR-012）

- 唯一的奖励是**拨轮按下选对**时团团开心的那一下（`GameOutput::right`）。揭晓答案、长按回家、超时、语音都没有奖励。
- **成长记忆**：一局**玩完**（3 题都结束）且**至少答对一题**，生成 `GAME_COMPLETED{option = 游戏 id, source = Device}`（`GameCompletedMemoryEvent`）。游戏 id：内置题 = 1，游戏包 = 包头 `content_id`（100–65535）。中途回家、超时、被睡觉/语音/家长菜单打断都不生成。游戏从不写**喜好**。
- **板端**：和绘本一样只走原子账本（#64）的 `RecordMemory` → `pet_service_.PostMemory`，id 和日期由账本的 Pet 任务分配（本地事件 id、RTC 日）；没有旁路存储。日志 `PET_GAME_END ... growth=1 posted=1`。

## 5. 画面与刷新

```
┌──────────────────────────────────────────────┐
│  团团（反应）    ┌───────────┐        ●     │
│   ♥        ♥    │  题目卡    │        ◉     │  ← 进度点
│  (^ ^)          │ （形状/喇叭）│        ○     │
│                 └───────────┘              │
├────────────── kGameRailTop = 296 ────────────┤
│ ┌───────┐   ╔═══════╗   ┌───────┐          │
│ │ 答案1 │   ║ 答案2 ║   │ 答案3 │          │  ← 拨轮高亮 = 粗框
│ └───────┘   ╚═══════╝   └───────┘          │
└──────────────────────────────────────────────┘
```

- `SceneKind::Game`（新增，值 4），内容在 `SceneModel::game`（`GameView`，像家长菜单一样不进 32 位场景字）。
- 所有图片来自固件内置的 1-bit 图库（§7），用 `scene_model.cc` 已有的画笔画，没有文字，也不从卡上读位图。
- **刷新区域**：拨轮移动只改 y ≥ 296 的答案栏，计入 `RefreshPlanner` 的 rail 预算；团团反应、换题、进度只改 y < 296 的题目区，计入 pet 预算。进出游戏是场景切换 = 一次全刷（约 4.3 s）；局内全是局刷，到硬预算才会被规划器改成全刷。测试保证这两个区域互不越界、不画进状态栏的两个角框。
- 游戏画面不画状态栏（和阅读器、家长菜单一样，状态栏只在首页）。

## 6. 板端接入（`scripts/p1_integrate.py`）

- 首页动作条按下「游戏」：先开游戏菜单（V4-07，§13），菜单第一张卡才是下面的知识题；`GameStartAllowed()` 不在安静时段、电量不低时开始游戏；否则日志 `PET_GAME_REFUSED reason=quiet|battery|voice`（voice：XiaoZhi 正在连接或对话），团团照旧打招呼（`PetInputRouter` 对 Game 仍返回 Pickup 作为退路）。
- 游戏是**纯 UI**：不发任何 Pet 动作、不写 NVS；`last_pet_scene_` 保持首页画面；`GatedSceneMailbox::game_only` 让游戏期间只有 Game 画面上屏（和家长菜单的 `parent_only` 同一机制）。
- 结束的地方都会 `EndGame()` 先回首页：玩完/长按/超时、睡觉流程（PWR、闲置睡觉、电量耗尽）、家长组合键、早安画面、语音会话（XiaoZhi 不再空闲）、退出 Pet 模式。
- 游戏期间：不播团团短句、不响应体感手势、不做电量反应和动画、不打盹（PowerManager 视为忙）、首页选项名提示关闭。
- **声音**：游戏音频走同一个 `pet_prompt` 播放任务（`PromptRequest::game`），和选项名、短句一样受家长音量上限和安静时段上限约束（`PromptVolumePct()`）；新声音打断旧声音。游戏包里有录音就用录音，否则用内置提示音（`LoadGameClip`）。
- 游戏包在 `pet_prompt` 读完声音包后打开一次、全量 CRC 校验，日志 `PET_GAME_PACK state=ready|absent|invalid|no_sd`。FATFS `max_files` 从 3 调到 4（绘本 + 朗读 + 声音包 + 游戏包）。
- 日志：`PET_GAME_START src=pack|builtin id= seed= rounds=`、`PET_GAME_ROUND n=1/3 kind= options= answer=`、`PET_GAME_SELECT`、`PET_GAME_ANSWER n= pick= right= feedback=right|try_again|reveal correct=`、`PET_GAME_PLAY/QUEUED clip=`、`PET_GAME_END end=completed|home|idle|interrupted reason= played= correct= id= growth=`。

## 7. 图库（`kid_app/game_view.h` `GameArt`，`tools/game_pack.py` `ART`）

id 一经发布不得改号、不得复用，只能追加；测试保证两边一致。

| 名字 | id | 名字 | id |
|---|---:|---|---:|
| circle 圆 | 1 | dots1..dots5 1–5 个圆点 | 11–15 |
| square 方 | 2 | blocks1..blocks5 1–5 块积木 | 21–25 |
| triangle 三角 | 3 | ball_big 大球 / ball_small 小球 | 31 / 32 |
| star 星 | 4 | cat 猫 / dog 狗 / bird 鸟 / fish 鱼 | 41–44 |
| heart 心 | 5 | listen 喇叭（**只能做题目图**） | 90 |

## 8. KPG1 文件格式

KPB2 给游戏保留了包类型 3，但 KPB2 的头部、页表都是为绘本设计的；游戏包用**自己的魔数 `KPG1`**（KPB2 规格 §2 允许），和 KPS1 声音包是同一种“表 + 音频段”的结构。清单里的 `package_type: "game"` 沿用 KPB2 的类型名。全部小端。

```
[0 ] 头部   32 B   <4sHHHHIIIII
[32] 题目表 题数 × 24 B
[..] 音频表 段数 × 16 B
[..] 音频段 PCM16，每段起点 4 字节对齐，填充为 0，最后一段结束 = 文件末尾
```

头部：

| 偏移 | 类型 | 字段 | 规则 |
|---:|---|---|---|
| 0 | 4s | magic | `KPG1` |
| 4 | u16 | version | 1 |
| 6 | u16 | round_count | 1–64 |
| 8 | u16 | clip_count | 1–128 |
| 10 | u16 | content_id | 100–65535（1–99 留给固件内置题）；写进成长记忆 |
| 12 | u32 | revision | ≥ 1 |
| 16 | u32 | sample_rate | 24000 |
| 20 | u32 | total_bytes | = 文件大小，≤ 8 MiB |
| 24 | u32 | tables_crc | 题目表 + 音频表的 CRC32 |
| 28 | u32 | header_crc | 前 28 字节的 CRC32 |

题目（24 B，`<BBBBH3HH3HI`）：kind（1–5）、option_count（2–3）、correct（1-based）、flags（0）、question_art（0 = 喇叭，或图库 id）、3 个答案图（图库里的答案图，同题不重复，未用的为 0）、prompt_clip、3 个 option_clip（16–999，未用的为 0）、保留 u32（0）。

音频表（16 B，`<HHIII`）：id（升序）、0、offset、bytes（偶数，2 B–10 s）、CRC32。id 2–5 是可选的整局台词（2 答对、3 再想想、4 揭晓、5 再见，缺了用内置提示音）；16–999 是题目和答案名录音，**必须被某道题用到**，题目引用的 id 必须存在。

读取器（`GamePackReader::Open`）在打开时逐项检查以上规则并校验每一段 CRC，有一处不对整包拒绝；CRC 只防意外损坏，不是签名。**包里没有任何文字**：题干和选项名只在旁路清单里给家长看。

## 9. 制作与审核

源目录：

```
animal_sounds/
  game.json
  q_moo.wav  cat.wav  dog.wav  cow.wav ...   24 kHz 单声道 PCM16，0.1–10 s
  right.wav  try_again.wav  reveal.wav  bye.wav   （可选）
```

```json
{
  "schema_version": 1, "package_type": "game",
  "game_id": "animal_sounds", "content_id": 201, "revision": 1,
  "title": "听声音猜动物", "reviewer": "妈妈", "approved": true,
  "session_audio": {"right": "right.wav", "try_again": "try_again.wav"},
  "rounds": [
    {"kind": "animal_sound", "prompt": "喵喵喵，是谁在叫？", "prompt_audio": "q_meow.wav",
     "question_art": "listen", "answer": "cat", "approved": true,
     "options": [
       {"art": "cat", "label": "小猫", "audio": "cat.wav"},
       {"art": "dog", "label": "小狗", "audio": "dog.wav"},
       {"art": "bird", "label": "小鸟", "audio": "bird.wav"}]}
  ]
}
```

- 字段集合是严格的；每道题和整个游戏都要 `"approved": true`，编译时还要 `--reviewed`。同一个 wav 被多道题用时只存一份。
- `reviewer`、题干、选项名只进旁路清单 `*.manifest.json`。

```
python -m tools.game_pack pack   animal_sounds/game.json --output out/default.kpg --reviewed
python -m tools.game_pack verify out/default.kpg
python -m tools.game_pack demo   --output build/game-smoke     # 仅提示音，冒烟测试用，不是儿童内容
```

拷到 TF 卡 `/games/default.kpg`。家长审核清单：

1. 每道题只有一个对的答案，答案卡上的图和录音说的是同一个东西；
2. 题干录音说清楚要找什么（“哪个是小猫？”），不催促、不批评；
3. 答对/再想想/揭晓/再见四句台词语气温和（可以不录，用内置提示音）；
4. 用 `manifest.json` 逐题核对图、答案、录音文件，确认 `placeholder: false`。

## 10. 测试

| 文件 | 覆盖 |
|---|---|
| `tests/cpp/test_mini_game.cc` | 内置题合法；种子确定性；高亮移动与夹紧；答对唯一奖励、反应期间按键无效；两次答错揭晓无奖励；整局完成、长按、超时（含 12 s 重复题目、时间回绕）、打断；成长记忆事件只在完成且答对时生成、能写进 `MemoryStore`、语音来源被拒；提示音；画面区域（高亮只动答案栏、反应只动题目区、状态角框无墨、每张图都在卡内）；刷新规划与闸门；自测种子 4242 的题序 |
| `tests/cpp/test_game_pack_reader.cc` | 读工具生成的黄金包：题目字段、录音逐字节一致、整局台词回退提示音、损坏包整包拒绝、TF 路径规则（CI） |
| `tests/test_v4_game_pack.py` | 源清单 20 多种错误；审核开关；清单里有文字、包里没有；黄金包可复现；篡改（含重新封好 CRC 的语义错误、非零填充）被拒；图库/种类/台词 id 与 C++ 头文件一致；`demo`/`verify` 命令 |
| `tests/test_p1_integration_contract.py` | 板端接线：启动条件、只放 Game 画面、各种打断先 `EndGame`、游戏中不播短句/不做手势/不打盹、同一音量路径、自测 `game` 步骤 |
| `tools/selftest/v4_mini_game.pettest` | 真机无人值守：内置题 3 局（完成 / 长按回家 / 超时），只用拨轮步骤（先过 V4-07 游戏菜单，选第一张卡） |
| `tests/cpp/test_mic_games.cc`（V4-08，§12） | 包络判定（合成吹气/拍手/说话/噪声）、吹蜡烛与拍手节奏会话、成长记忆、画面刷新区域 |
| `tests/test_v4_mic_games.py`（V4-08，§12） | 隐私契约（判定模块无文件/网络/缓冲区）、AudioService 补丁只给一个数字、板端只在游戏里开麦克风、语音互斥、自测步骤 |
| `tools/selftest/v4_mic_games.pettest`（V4-08，§12） | 真机无人值守：脚本代替麦克风注入合成包络，三局（拍手 / 吹蜡烛 / 干扰后长按回家） |

黄金包 `tests/fixtures/kpg1/golden_game.kpg` 是合成提示音，**不是儿童内容**；格式有意变更时用 `python tests/test_v4_game_pack.py --write-golden` 重新生成。

## 11. 没有做的

- 真机未验证：画面、局刷耗时与残影、提示音音量、TF 包加载耗时都等协调会话合并后跑 `v4_mini_game.pettest`。
- 颜色题（墨水屏只有黑白）；游戏包自带位图（以后要加需换版本号）；体感游戏（V4-07）。麦克风游戏见 §12。
- 动物、英语单词、大小比较三类只有格式和工具支持，没有现成的审核内容和录音。

## 12. V4-08：麦克风游戏（吹蜡烛、拍手节奏）

对应 `DEVELOPMENT_PLAN.md` V4-08、`PRODUCT_REQUIREMENTS.md` §2 第 7 条、ADR-011。放在上面的小游戏框架里：同一个游戏场景（`SceneKind::Game`）、同一套 `GameOutput`/`GameResult`/`GameEnd`、同一条奖励和成长记忆规则、同一条声音通路和 `GameActive()` 保护。

### 12.1 隐私约束（硬性）

- **只用麦克风的音量包络**（V1-10）：音频输入任务每读到一帧 10 ms 的输入，就算出**一个 RMS 数字（uint16）**交给判定器，**帧本身马上丢掉**。
- **不录音、不落盘、不上传**：没有任何地方复制、缓存、编码、写文件、写 NVS、进发送队列或走网络；判定模块（`mic_envelope.*`、`mic_game.*`）里没有文件、flash、网络和样本缓冲区的任何代码。
- 契约测试 `tests/test_v4_mic_games.py` 把这些写成会失败的检查：判定模块里出现 `fopen`/`ofstream`/`nvs_`/`http`/`socket`/`std::vector`/`pcm` 等就报错，`#include` 只允许 `kid_app/` 和几个纯标准头；AudioService 补丁新增的代码不得出现任何队列、编码、文件和网络调用，板端新增的整段不得出现存储或网络调用。
- **只在游戏真的要听的时候开**：`MicGameSession::MicWanted()` 为真才打开麦克风通路（吹蜡烛：整局；拍手：团团演示完、提示音播完、再留 0.4 s 让判定器适应环境之后），游戏一结束（不论什么原因，`EndGame`）立刻关掉。
- **与小智语音互斥**：只在小智空闲时才能开始游戏；BOOT 说话（小智不再空闲）会 `EndGame("voice")` 并关掉麦克风；AudioService 补丁里 `EnableVoiceProcessing(true)` 也会清掉这条通路。通路本身还是小智那唯一的输入任务在读，**没有第二个 I²S 读者**。
- 团团自己的声音（开始/蜡烛熄灭/节拍/“该你了”）会被麦克风听到：吹蜡烛在每个提示音后 0.7 s 内不听；拍手游戏在演示节拍和“该你了”提示音期间麦克风是关着的。

### 12.2 包络判定（`MicClassifier`，纯 C++）

输入每 10 ms 一个 RMS 值，输出 `blowing`（吹气进行中）和 `clap`（确认一次拍手）。先用前 0.3 s 学房间底噪（floor），之后一次“声响”= 超过 `max(3×floor, floor+600)` 起、低于 `max(2×floor, floor+300)` 连续 30 ms 止：

| 声音 | 判定 | 区别 |
|---|---|---|
| 拍手 | `clap` | ≤ 150 ms，峰值 ≥ `max(3000, 8×floor)`，前 30 ms 就到峰值的 60 % 以上 |
| 吹气 | `blowing` | ≥ 250 ms，平均 ≥ `max(1500, 5×floor)`，几乎没有低于均值一半的凹陷（≤ 1 次） |
| 说话 | 都不是 | 音节之间有凹陷：声响很短（< 250 ms）或反复凹陷；高声说话峰值也达不到拍手的“一上来就最响 + 很短” |
| 环境噪声 | 都不是 | 不超过开启线，被慢慢学成 floor；持续 > 6 s 的响声（风扇、电视）整体并入 floor |

门限都是**初值**，由 V1-10 的真机数据校准（已知局限：纯 RMS 区分不了“一直拖长的元音”和“吹气”，只能靠平均音量和凹陷；孩子贴近麦克风吹气是设计假设）。

`SynthEnvelopeRms(MicSynth, t_ms)` 给出吹气、拍手、说话、噪声的**确定性合成包络**，主机单测和硬件自测共用。

### 12.3 两个游戏（`MicGameSession`）

| 游戏 | 玩法 | 奖励 / 成长 |
|---|---|---|
| 吹蜡烛（`candle`，成长 id 2） | 3 根点着的蜡烛，对着麦克风**稳稳吹** 0.7 s 灭一根（吹到中途停下会慢慢扣回去，每灭一根后 0.7 s 不听）；3 根都灭了团团开心，2.5 s 后回首页。30 s 没人吹回首页 | 灭完最后一根 = 唯一的奖励；成长记忆（`GAME_COMPLETED`，option = 2） |
| 拍手节奏（`clap`，成长 id 3） | 团团用提示音敲 3 下（间隔 700/700 ms），“该你了”提示音后，孩子拍 3 下**同样的节奏**（每段间隔与示范相差 ≤ 250 ms 算对）；第 2 轮间隔 500/1000 ms。听 7 s，拍了一下后 2.5 s 没有下一下算错。每轮最多 2 次：第一次错“再想想”并重放，第二次错“揭晓”（不算对）进入下一轮 | 每轮拍对 = 一次奖励；两轮玩完且至少对一轮 = 成长记忆（option = 3） |

- 屏幕：团团 + 题目卡（吹气脸 / 拍手），答案栏位置显示 3 根蜡烛（点着/熄灭）或 3 个手（等待/已拍到）；**没有拨轮高亮**（`highlight = 0xFF`），只有长按回首页有效。画面只用已有画笔，新增 5 张图（`MicArt` 101–105，不在 KPG1 图库里，游戏包不能用）。
- 刷新区域遵守 §5：蜡烛熄灭/拍到一下只改 y ≥ 296 的答案栏，团团反应只改题目区。
- 内置提示音复用 `mini_game.cc` 里已有的音色（不新增声音数据）。
- 入口：首页「游戏」不变。**真实使用**时按 `quiz → 吹蜡烛 → quiz → 拍手` 轮着来（`PickMicGame`，开机后第 2、4 次开始游戏是麦克风游戏）；自测脚本里写了 `game <seed>` 就一直是问答，用 `micgame` 步骤强制。

### 12.4 板端

- `scripts/v4_mic_tap_patch.py`（在 `p4_audio_queue_patch` 之后应用）：`AudioService` 新增 `SetMicEnvelopeTap(enable, sink, context)` 和事件位 `AS_EVENT_MIC_ENVELOPE`；`AudioInputTask` 在已有读帧位置算 `MicFrameRms`（取第 0 声道）并调用 `sink(context, rms)`；有语音引擎在跑时复用同一帧，不额外读。
- `p1_integrate.py` overlay：`mic_game_`/`mic_session_`（游戏入口 `StartMicGame`，`PublishGameSceneLocked`、`HandleGameInput`、`UpdateGame`、`EndGame` 按 `mic_session_` 分流）；`ArmMic`/`DisarmMic`/`UpdateMicGame`。日志：`PET_GAME_START src=mic id= kind= rounds=`、`PET_MIC_TAP armed=1 src=audio|script`、`armed=0`、`PET_MIC_HEARD kind=blow|clap`、`PET_MIC_CANDLE lit=`、`PET_MIC_CLAPS round= claps= feedback= right=`、`PET_GAME_END ...`（和问答同一格式，`growth=1 posted=1`）。
- **没有**把“麦克风游戏开关”放进家长菜单：本版只有一个 `motion_enabled`（体感）；如要给麦克风单独的家长开关需要加设置字段，留给后续。

### 12.5 自测（`tools/selftest/v4_mic_games.pettest`）

`PETSELFTEST1` 新增两个步骤（`selftest_script.h` 有说明）：

| 步骤 | 作用 |
|---|---|
| `micgame <candle\|clap>` | 下一局是这个麦克风游戏；同时把“麦克风”交给脚本（之后真麦克风通路保持关闭） |
| `envelope <blow\|clap\|speech\|noise> <ms>` | 这 <ms> 毫秒里，判定器听到的是合成包络（只是音量数字，不是音频）；`clap` 在步骤开始时拍一下，之后安静 |

场景：游戏 1 拍手（两轮都拍对）→ 游戏 2 吹蜡烛（先说话、拍手、噪声都无效，再吹 5 s 灭 3 根）→ 游戏 3 吹蜡烛（同样的干扰后长按回首页）。主要期望：`none:src=audio 0`（真麦克风通路一次都没开）、`min:src=script 4`、`min:end=completed 2`、`min:end=home 1`、`min:growth=1 2`、`min:growth=0 1`、`min:correct=2 1`、`commits 0`。

**建议监视秒数**：`python tools/selftest_report.py plan tools/selftest/v4_mic_games.pettest` → 预计到 `PET_SELFTEST_DONE` 约 114 s，上限 124 s（脚本约 101 s）。运行条件同 v4_mini_game：不在安静时段、电量不低、不放 TF 游戏包也行、提示音 `volume 15`。

### 12.6 没有做的 / 待真机

- 真机未验证：真麦克风的包络数值（`SetMicEnvelopeTap` 路径、声道/重采样后的幅度、拍手和吹气的实际门限，需要 V1-10 数据校准）、扬声器回声、局刷耗时。自测只覆盖“判定器 + 会话 + 画面 + 日志”，不经过真麦克风（刻意：`none:src=audio`）。
- 真机上的最小手测：进游戏后对着麦克风吹 1 s / 拍手，看 `PET_MIC_HEARD`；确认 BOOT 说话时 `PET_MIC_TAP armed=0`。

- 颜色题（墨水屏只有黑白）；游戏包自带位图（以后要加需换版本号）；麦克风游戏（V4-08）。体感游戏见 §13。
- 动物、英语单词、大小比较三类只有格式和工具支持，没有现成的审核内容和录音。

## 13. V4-07 体感游戏：倾斜滚球、扣下躲猫猫

对应 `DEVELOPMENT_PLAN.md` V4-07，`PRODUCT_REQUIREMENTS.md` §3（只奖励“轻”动作，剧烈摇晃不加分，可关闭）、§7，ADR-011（只在本地算特征，不存原始数据）、ADR-012（奖励与成长记忆）。复用 V4-06 的游戏壳（`GameOutput` / `GameResult` / `GameView` / 提示音 / `PromptRequest::game` / 成长记忆）和 V4-01/02 的手势识别（`GestureDetector`），纯逻辑在 `kid_app/motion_game.{h,cc}`，不新增任何存储。

### 13.1 玩法

首页「游戏」→ **游戏菜单**：5 个游戏，每次显示 3 张图卡，窗口跟着高亮滑动（星星 = 知识题，球 = 倾斜滚球，小猫 = 扣下躲猫猫，蜡烛 = 吹蜡烛，手 = 拍手节奏，后两个是 V4-08）。拨轮上下选、到头夹紧、按下开始、长按回首页、60 s 没操作回首页。V4-08 原来的“知识题/吹蜡烛/知识题/拍手”轮换取消，改由孩子在菜单里选；自测的 `micgame` 步骤仍直接开始吹蜡烛/拍手（绕过菜单）。

| 游戏 | 孩子怎么玩 | 奖励 |
|---|---|---|
| **倾斜滚球** | 屏幕下方一条 7 格的轨道，球在中间（第 3 格），洞在两端附近（第 0/1/5/6 格，每局种子决定）。把板子向左/右倾斜，球每 0.5 s 滚一格（倾斜保持着就一直滚，到头停住不掉出去）；球滚进洞团团开心，2.5 s 后下一轮，共 3 轮 | 球进洞 = 团团笑 + 爱心（唯一的奖励） |
| **扣下躲猫猫** | 对孩子说“把团团藏起来”：把板子屏幕朝下扣在桌上（团团闭眼），**至少 2 s** 后翻回来，团团“找到你啦”，3 轮 | 藏够 2 s 再翻回来 = 开心 + 爱心；翻太早只说“再来一次”，没有奖励也没有惩罚 |

- **剧烈摇晃永远不是奖励、也不是事件**：滚球里只有倾斜/水平被听到，躲猫猫里只有扣下/翻回被听到，轻摇、重摇、拿起等都被游戏忽略（日志 `PET_GESTURE ... acted=0`）。
- 长按拨轮回首页；滚球 60 s 没倾斜（或躲猫猫等扣 60 s 没动）重复一次提示音，之后回首页；躲猫猫扣着不翻最长 120 s。BOOT（说话）和 PWR（睡觉）和知识题一样结束游戏。
- 家长菜单里关了“体感”：菜单里选这两个游戏会被拒绝（`PET_GAME_REFUSED reason=motion_off`），停在菜单；知识题不受影响。
- 声音全是固件内置提示音（开始/进洞/找到/再来一次/再见），没有录音，也走同一个音量上限和安静时段规则。
- 每局玩完且至少赢一次，生成 `GAME_COMPLETED`，游戏 id：知识内置题 = 1，**滚球 = 4，躲猫猫 = 5**（2、3 是 V4-08 的吹蜡烛、拍手节奏，1–99 留给固件内置，包的 id 从 100 起，不会撞）；中途回家、超时、被打断都不生成。只走原子账本的 `RecordMemory`（和 V4-06 相同），从不写喜好。

### 13.2 逻辑与画面

- `TiltTracker`：把 500 ms 的 IMU 窗口均值变成 `TiltLeft` / `TiltRight` / `Level`（`Gesture` 新增这三个值，只给滚球用，`GestureDetector` 不报）。屏幕朝上（z ≤ −500 mg）、窗口内晃动 < 500 mg、所选轴均值 > 300 mg 才算倾斜；回到 ±150 mg 内报一次水平。
  - **轴向和正负号是 `TiltConfig`（`use_y`、`sign`）：默认用 x 轴、正为右。板子横拿时哪一个轴对应孩子的“左右”要在真机上标定**（QMI8658 的安装方向没有在文档里确认），不对就改这两个常量即可。
- `MotionGameSession`：滚球 / 躲猫猫的状态机（和 `GameSession` 一样确定性、单一所有者、时间戳回绕安全，空闲/重复提示计时接 `SelfTestClock`；2 s 的“藏够”是物理动作，不缩放）。
- `GameMenu`：菜单，复用 `IconChoice`。
- 画面仍是 `SceneKind::Game`：`GameView` 增加 `motion`（1 滚球 / 2 躲猫猫）、`ball`、`hole`、`hidden`；菜单用原来的答案卡画法（星/球/猫/蜡烛/手五张图，三张一屏）。滚球的轨道和球在 y ≥ 296 的答案栏区域（球移动 = 只改这一块，计入 rail 局刷预算）；团团反应、进度点在上方（pet 预算）；躲猫猫扣下时上方团团闭眼、下方一张空心手机卡变实心。新画面同样不画进状态栏的两个角框，测试保证区域不越界。

### 13.3 板端接入（`scripts/p1_integrate.py`）

- `StartGame()` 现在只开菜单（`OpenGameMenu`），菜单选择后 `StartQuiz()`（原 V4-06 的启动）或 `StartMotion()`。`game_mode_`（None / Menu / Quiz / Motion）决定谁拿拨轮和画面；`game_active_` 对菜单和所有游戏都为真，所以 V4-06 加的那些“游戏中不播短句 / 不做动画 / 不打盹 / 不电量反应”守卫全部照旧生效。
- 手势：`HandleGesture` 在动体感游戏时把 FaceDown / FaceUp / TiltLeft / TiltRight / Level 放进一个原子字，`UpdateGame`（pet_input，20 ms 一次）取出交给 `MotionGameSession`；`PET_GESTURE kind=… acted=1 game=1`。自测的 `gesture` 步骤走同一条路径。
- 倾斜只在滚球进行时由 `pet_boot_diag` 的 IMU 窗口喂给 `TiltTracker`（`motion_tilt_` 且家长未关体感），其余时间复位；躲猫猫用现有 `GestureDetector` 的 FaceDown/FaceUp（1 s 持续判定）。不新增任何采样、存储或上传。
- 日志：`PET_GAME_MENU highlight=`、`PET_GAME_MENU_SELECT`、`PET_GAME_CHOICE game=quiz|ball|peekaboo|candle|clap`、`PET_GAME_START src=motion kind=ball|peekaboo id= seed= rounds=`、`PET_MOTION_ROUND n=1/3 kind= ball= hole=`、`PET_MOTION_STEP ball= hole=`、`PET_MOTION_HOLE`、`PET_MOTION_HIDE`、`PET_MOTION_TOO_SOON`、`PET_MOTION_FOUND`、`PET_GAME_END mode=menu|quiz|ball|peekaboo end=… growth=…`。

### 13.4 自测与真机检查

- `tests/cpp/test_motion_game.cc`：倾斜判定（方向、保持、水平只报一次、晃动/扣着不算、轴和符号可配）；滚球确定性与边界、不掉出轨道、反应期不动、不重复得奖；躲猫猫太早 / 藏够 / 摇晃无奖励 / 超时；成长记忆只在完成且赢过时生成；菜单；画面区域和刷新闸门。`scripts/p2_test.sh` 末尾新增一块。
- `tests/test_p1_integration_contract.py`：菜单先于游戏、手势只转发 5 个游戏手势且不含摇晃、倾斜只在滚球时喂、`EndGame` 清所有标志、不碰存储、自测 `gesture` 复用 `HandleGesture`。`tests/test_selftest_report.py`：新场景的 `plan` 与期望能被报告工具识别（含失败路径）。`tests/cpp/test_selftest_script.cc`：`gesture tiltleft|tiltright|level`。
- `tools/selftest/v4_motion_games.pettest`（种子 4242 的洞依次在第 6、0、5 格）：长按退出菜单；滚球 3 轮（8 次倾斜）；躲猫猫 1 次翻太早 + 3 次找到，中间一次重摇无奖励；两局完成、`growth=1` 两次、无提交。`gesture` 步骤只测接线，**阈值和轴向要靠人拿着板子真倾斜、真扣下来验证**（真机未验证）。
- 已知影响：`v4_mini_game.pettest` 多了一步“菜单里按下第一张卡”。
