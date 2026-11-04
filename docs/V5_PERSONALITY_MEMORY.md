# V5-01..03：个性与记忆（纯 C++ 核心）

对应 `DEVELOPMENT_PLAN.md` V5-01/02/03、ADR-012、`PRODUCT_REQUIREMENTS.md` §8、`INTERFACES_AND_DATA.md` §5.1。
代码在 `firmware/components/kid_app`，全部是无 I/O 的纯 C++，由 `scripts/p2_test.sh` 在 CI 里用 `g++ -Wall -Wextra -Werror` 跑。

## 1. Personality（V5-01）

`include/kid_app/personality.h`

- 三种性格 `Temperament`：`Curious` 好奇 / `Gentle` 温柔（默认）/ `Lively` 活泼。由家长设定，模型不能改。
- `PersonalityParams`：口头禅（话术模板 id）、默认表情倾向、首页五项（聊天/读书/游戏/喂食/睡觉）的基础提议权重。好奇偏读书，温柔偏聊天，活泼偏游戏。
- 话术库：36 条审核过的中文模板（3 性格 × 6 场景 × 2），每条有稳定 id：`(性格+1)*100 + (场景+1)*10 + n`。
  场景：问候、早安回顾、提到喜好、喂食、邀请读书、睡前。模板只含 `{book}` / `{like}` 两种记忆槽位，没有自由文本生成。
  **已发布的 id 含义不得修改**，新增文案用新 id（音频片段和家长审核都按 id 引用）。
- `SelectPhrase(...)`：只在槽位可用时选带槽位的模板（有读过的书才会说《{book}》，有选过的喜好才会说{like}）；带槽位模板权重 3、通用模板权重 1；每个“性格×场景”都有通用兜底句，所以合法输入一定返回一个 id。
- `Propose(temperament, DayPhase, seed)`：按日相调整权重后挑一个首页提议。夜间/午睡只提议睡觉；早上喂食 +2；傍晚读书 ×2、不提议游戏、提议睡觉。
- 确定性：没有随机源，只用调用方传入的 `seed`（例如账本 generation）经固定整数哈希选择。**同输入同输出**，测试里还钉了几个跨编译器的黄金值。

## 2. MemoryStore（V5-02）

`include/kid_app/memory_store.h`，固定 288 字节小端编码。

| 层 | 内容 | 上限与淘汰 |
|---|---|---|
| 今日 `Today()` | 日期（本地日序号）、喂食/问候/读完书/完成游戏/选择题次数、最后一本书、最后一个游戏 | 每项饱和到 250；换日时整体归档 |
| 昨日 `Yesterday()` | 上一个**有活动**的日子（给 V5-04 早安回顾用） | 只保留 1 天 |
| 喜好 `PreferenceAt()` | `{slot, option, source_event}`，slot/option 是内容包白名单 id | 16 条；同一 slot 重答则覆盖并移到最前；满了淘汰最久没选过的 |
| 成长 `GetGrowth()` | 读完书数、完成游戏数、选择次数、活跃天数、里程碑位 | 计数饱和到 65535；里程碑只增不减（清空除外） |
| 最近书目 `RecentBookAt()` | 最近读完的书 id，新的在前 | 8 本；重读移到最前；满了丢最旧的 |

- 换日规则：事件带 `day`（本地日序号，0 = 时钟未知）。`day` 更大 → 归档今天、开新的一天；`day` 相同或 0 → 计入今天；`day` 更小（时钟回拨）→ 只计入成长/书目/喜好，不改今天。时钟未知时产生的活动归到第一次知道日期的那一天。
- 里程碑：第一本书、5 本、10 本、第一个游戏、10 个游戏、第一次选喜好、活跃 7 天。
- 解码严格且只接受规范形式：未用条目、填充、保留区必须为 0；重复 slot/书目、超容量、未知里程碑位、昨日不早于今日等都判为损坏。

## 3. 写入路径守卫（V5-03）

所有写入都走 `MemoryStore::Apply(MemoryEvent)`，事件只有结构化字段（种类、来源、日序号、slot、option），没有任何文本。

| 事件 | 允许的来源 | 说明 |
|---|---|---|
| `PreferenceChosen`（PREFERENCE_CHOSEN） | **仅 `DialChoice`** | 写喜好的唯一路径 |
| `BookFinished` / `GameCompleted` | `DialChoice` / `Button` / `Device` | 只能由设备上的规则产生 |
| `MemoryCleared`（MEMORY_CLEARED） | **仅 `Parent`** | 一键清空 |
| `TemperamentSet` | **仅 `Parent`** | 家长设定性格 |
| 任意事件 | `Voice` / `Network` 一律拒绝 | 不从自由语音或未鉴权数据写记忆 |

另外：事件 id 为 0、slot/option 为 0、性格越界、未知种类/来源都拒绝；被拒绝的事件不改变任何状态。
没有任何其他 API 能写喜好。

**家长清空**：清掉今日、昨日、喜好、成长、最近书目，回到“刚认识”；**性格保留**；宠物状态（喂食数、模式）和外发队列不受影响。

**给语音服务的最小摘要** `MakeMinimalSummary()` / `FormatMinimalSummary()`：只含性格、喜好的 `{slot, option}`、今日四个计数、最近一本书 id、里程碑位，输出紧凑 JSON，例如：

```json
{"trait":"gentle","preferences":[{"slot":2,"option":31}],"today":{"feed":2,"greet":0,"books":1,"games":0},"last_book":17,"milestones":33}
```

不含事件 id（审计用的 `source_event`）、日期、名字或家长备注。

## 4. 进入账本事务：PetAtomicLedger schema v2

- `PetAtomicLedger` 的单块 NVS blob 从 v1（1,480 字节）升级到 v2（1,768 字节）：原有 28 字节头 + PetCheckpoint + 16 条外发队列不变，在 CRC 之前追加 288 字节记忆块；头里的版本号 1→2、长度字段同步更新。
- `RecordMemory(MemoryEvent)`：校验 → 与宠物状态、外发队列一起**一次提交**。记忆事件与宠物事件共用同一个 128 条重放 id 窗口，所以同一个 id 重放（断电重启后、或两种事件撞 id）返回 `Duplicate`，**绝不重复计数**。写入失败时内存状态不变，同一 id 可重试。
- `ApplyDurably(PetEvent)` 在同一次提交里给今天的喂食（Feed）/问候（Pickup）计数，重复事件同样不计。
- **迁移**：`Store::Read` 现在返回实际长度。读到 1,480 字节的 v1 blob 时按 v1 严格校验（魔数、版本 1、长度、CRC、规范编码），宠物状态、重放窗口和外发队列原样恢复，记忆为空（v1 没有记忆）。下一次提交把 v2 写到另一个槽位，**v1 槽位保持原样**作为回滚副本；不擦除任何数据。其他长度一律按损坏处理（不自动擦除）。
- 旧的 `kid_pet` / `PetCheckpointCodec`（板上当前在用的 legacy 存储）没有改动，字节格式不变。

## 5. 测试

| 文件 | 覆盖 |
|---|---|
| `tests/cpp/test_personality.cc` | 话术表 id 规则/唯一/槽位标记/兜底句；同输入同输出 + 黄金值；夜间/午睡只睡觉、傍晚无游戏、白天不睡觉；三种性格的偏好分布；无记忆时绝不选槽位句，有记忆时多数选槽位句 |
| `tests/cpp/test_pet_memory.cc` | 非拨轮来源写喜好全部拒绝、语音/网络写任何记忆都拒绝、家长才能清空/设性格；换日、时钟回拨；喜好 16 条 LRU、书目 8 本、每日饱和；里程碑；编解码严格性（12 种损坏）；清空保留性格；最小摘要不含 id/日期；v1→v2 迁移（独立构造的 v1 blob、v1 槽位保留、错版本/坏 CRC/怪长度拒绝）；重放（含重启后、跨事件类型）不重复计数；写失败回滚与重试；旧清空事件重放不会抹掉新记忆；记忆块损坏时回退到上一个槽位 |
| `tests/cpp/test_pet_atomic_ledger.cc` | 原有账本测试，适配新的 `Read` 签名与 v1/v2 尺寸断言 |

## 6. 尚未完成

- 板级接线：板子目前仍用 legacy `FreertosPetService`（`kid_pet`），`PetAtomicLedger` 是 opt-in，尚未在板上启用（见 `P3_ATOMIC_PET_LEDGER.md`）。启用后记忆才会真正写入设备 NVS；`FreertosAtomicPetService` 还没有投递 `MemoryEvent` 的入口。
- V5-04 记忆驱动话术（早安回顾、拿起时提到喜好）的真机流程、`{book}`/`{like}` 槽位到书名/选项名的渲染和音频片段。
- V4-04/05 页内选择题还没有产生 `PreferenceChosen` 的真实来源。
- V5-05 服务端记忆同步/查看/删除；记忆事件暂不进入外发队列。
- V5-06 家长初始化（名字、性格、声音包）的 UI 与 API。
