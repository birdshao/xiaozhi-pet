# V5/V6 服务端开发方案（PetLearning）

状态：**方案稿，待用户拍板 §13 的决策后开发**｜日期：2026-11-04｜基线：main 03295fb（设备端 #51–#69 已合并）

覆盖 [开发计划](DEVELOPMENT_PLAN.md) 的服务端任务：V5-05（记忆同步/查看/删除、给语音服务的最小摘要）、V5-06（家长初始化：名字、性格、声音包）、V6-02/03/04/05（固定命令、绘本当前页问答、个性作为角色设定、开放问答开关与审核失败关闭）、V6-06 的服务端部分（内容列表、记忆清空），以及内容/声音包分发。设备端的配套改动只列为依赖（§11.2），不在本方案里实现。

## 1. 现状盘点

### 1.1 已有（可直接复用）

| 模块 | 位置 | 能力 | 限制 |
|---|---|---|---|
| PetLearning API | `services/pet_learning/api.py` | FastAPI；`POST /v1/devices/{id}/events`（1–32 条批量、逐条 ACK）、`GET snapshot`、审核页 `GET /v1/books/{id}/pages/{n}`、测验/阅读器、`POST intent` | 只绑 127.0.0.1；无家长鉴权；无记忆、个性、策略、内容分发接口 |
| PetStore | `services/pet_learning/store.py` | SQLite；设备 token 只存 SHA-256、`hmac.compare_digest`；事件日志按 `(device_id, message_id)` 永久回执，同 id 同内容 → `DUPLICATE` 返回原版本，内容变了 → `MESSAGE_ID_CONFLICT`，跳号 → `SEQUENCE_GAP_OR_REPLAY` | 建表用 `CREATE TABLE IF NOT EXISTS`，没有 schema 版本和迁移；事件枚举只有 9 种 |
| 意图 | `services/pet_learning/intent.py`、`quiz.py`、`story_reader.py` | 固定短语 → `local_command` 提议；当前页问答只回审核页原文；未知话语固定兜底，绝不回显 ASR | 没有"几点了/电量/睡觉/讲故事"；页上下文来自服务端投影的 `mode/active_book`，而设备的故事事件并没有上报 |
| 语音守卫 | `integrations/xiaozhi_server/child_voice_guard.py` + `scripts/p3_server_overlay.py` | 儿童模式接管每一句 ASR；只播固定话术或审核内容；API 失败播固定句，**永不回落到 LLM**；只允许回环地址 | 只覆盖 ASR→意图这一条 TTS 路径；欢迎语/报错/告别等其他 TTS 来源未审计 |
| 管理 CLI | `tools/pet_learning_admin.py` | `enroll`（token 从环境变量读）、`import-page`、`import-quiz` | 内容按"单页文本"导入，与设备实际用的 KPB2 包没有绑定 |
| 设备传输 | `esp_pet_https_transport`、`freertos_pet_http_worker`（[P3_ESP_HTTPS_SYNC](P3_ESP_HTTPS_SYNC.md)） | 证书校验 HTTPS、拒绝明文/重定向、逐条 ACK 校验 message_id+seq、1→60 s 退避 | **板上未启用** |
| 原子账本 | `PetAtomicLedger` schema v3（[P3_ATOMIC_PET_LEDGER](P3_ATOMIC_PET_LEDGER.md)） | 宠物快照 + 288 B 记忆 + 28 B 家长设置 + 16 条外发队列同一次 NVS 提交；记忆/设置/宠物事件共用 128 条重放窗口 | 板上 `SetProjection(false)`：不写外发队列，序号游标停在 1；**记忆事件从不进外发队列** |

测试：`tests/test_p3_pet_learning_api.py`（鉴权隔离、跨重启恰好一次、跳号、只读审核书、不回显语音、安静时段、无公开管理接口）、`tests/test_p3_xiaozhi_child_voice_guard.py`（非儿童模式无操作、错板/缺配置失败关闭、网络失败固定句、非回环地址拒绝、overlay 严格）。

### 1.2 缺口

1. **记忆上不了服务端**：记忆事件不进外发队列，外发队列只有 16 条且 NVS 只剩约 72 个空闲条目（ADR-014 补充），不可能为记忆再加队列。
2. **没有家长身份**：现有接口只有设备 token；"家长查看/删除/初始化"无从鉴权。
3. **没有服务端→设备的通道**：清空记忆、设置性格、改策略都需要下发，目前只有设备→服务端。
4. **内容两套来源**：设备读 TF 卡上的 KPB2/KPS1/KPG1，服务端读 CLI 导入的单页文本，二者没有哈希绑定，"当前页问答只用审核文本"无法保证说的是孩子眼前那一页。
5. **语音拿不到设备上下文**：设备没上报当前书页、电量；`intent` 用的是服务端自己的投影，和板上实际状态不一致。
6. **开放问答完全没有**：无审核端口、无角色设定、无熔断。
7. **部署只到回环**：没有 TLS 入口、配对、token 轮换、限流、备份。

## 2. 设计原则（沿用 ADR，不新增例外）

- **设备是宠物状态和记忆的唯一权威**（ADR-002/012/014）。服务端只存投影副本，绝不"回灌"奖励或计数。
- **程序管动作、模型管语言**（PRD §2.2，ADR-008）。语音只能"提议"固定枚举动作，设备 PetCore 决定是否执行；模型不能写记忆、不能发奖励。
- **审核前不发声**（ADR-006）：任何进入 TTS 的文本要么是固定话术/审核内容，要么完整通过输出审核；审核不可用 = 关闭开放问答。
- **数据最小化**：不存录音、不存 ASR 原文、不存模型回复原文；记忆只有结构化数字 id。
- **离线优先**：服务端不可用时设备照常工作；所有同步都可以任意延迟、重复、乱序到达。

## 3. 部署形态

### 3.1 推荐：家庭局域网单机自托管

```mermaid
flowchart LR
    DEV["ESP32 宠物"] -- "WSS 语音（小智协议）" --> PX
    DEV -- "HTTPS 业务（设备 token）" --> PX
    PAR["家长手机/电脑浏览器"] -- "HTTPS 家长面板（家长会话）" --> PX
    subgraph HOST["家庭主机（一台）"]
      PX["反向代理 Caddy：TLS 终止、限流、只开 443"]
      XZ["小智 Python 服务端（锁定 commit + overlay）"]
      PL["PetLearning（本仓库 services/）"]
      DB[("SQLite WAL\n+ 内容包目录")]
      PX --> XZ
      PX --> PL
      XZ -- "回环 HTTP + 语音 token" --> PL
      PL --- DB
    end
    XZ -. "ASR/LLM/TTS/审核 API（可外部）" .-> EXT["外部模型服务"]
```

- **两个进程、一台机器**：小智服务端（`xinnan-tech/xiaozhi-esp32-server`，锁定 commit，按 [P3_XIAOZHI_VOICE_BRIDGE](P3_XIAOZHI_VOICE_BRIDGE.md) 打 overlay）只负责语音链路；PetLearning 拥有全部业务数据。二者只通过回环 HTTP 通信，PetLearning 不读小智的数据库，小智不读 PetLearning 的 SQLite（ADR-001"可替换语音服务"）。
- **只在局域网**：反向代理只监听局域网地址；**不做公网暴露**，家长在家里用。外网访问列为待决策（§13 D3），默认不做。
- **形态**：推荐 Docker Compose（`deploy/compose.yaml`：caddy + xiaozhi-server + pet-learning，PetLearning 与小智同在 `network_mode: host` 或同一容器网络，守卫的地址白名单相应放开到配置的内部主机名）。也支持裸跑（uvicorn + Caddy 二进制）。数据目录、内容包目录可配置到非系统盘。
- **存储**：继续用 SQLite（WAL、`busy_timeout`、单写者），单家庭量级足够；不引入 Postgres/Redis。内容包文件按 `sha256` 命名放在 `data/packages/`。
- **备份**：`tools/pet_learning_admin backup` 做 SQLite 在线备份（`sqlite3.backup`）+ 内容目录，输出可加密归档，放家长指定位置；恢复演练进测试。

### 3.2 与小智服务端的关系

| 事项 | 归属 |
|---|---|
| WebSocket、Opus、VAD、ASR、TTS、设备 OTA 地址下发 | 小智服务端（上游，不改协议） |
| 儿童模式拦截每一句 ASR | overlay 钩子 `child_voice_guard`（变薄：只转发文本、播放返回的句子） |
| 意图、页问答、角色设定、开放问答、审核、熔断 | **PetLearning 新的 `/v1/voice/{id}/turn`**（§6）——安全逻辑集中在本仓库、可单测，不散落在上游代码里 |
| 小智自带的聊天记录、记忆模块、音频落盘 | **在儿童设备上全部关闭**（以锁定 commit 的配置项逐项核对并写进部署检查清单；S7） |
| 设备端动作（睡觉、翻页、开书） | 设备 PetCore；语音只提议（§6.2） |

## 4. 数据模型

### 4.1 迁移机制

`services/pet_learning/db.py`：用 `PRAGMA user_version` 记录 schema 版本，按序执行 `migrations/NNN_*.sql`，每次迁移一个事务；启动时版本高于代码认识的版本 → 拒绝启动（防降级损坏）。现有建表语句作为 `001_baseline`（对已有库是无操作）。每个迁移有"旧库 → 新库"测试。

### 4.2 表（新增）

| 表 | 关键字段 | 说明 |
|---|---|---|
| `devices`（扩展） | `+ status`（active/revoked）、`+ token_rotated_at`、`+ enrolled_at`、`+ fw_version`、`+ last_seen_at` | 保留 `token_sha`，只存哈希 |
| `pairing_codes` | `code_sha`, `device_id`, `expires_at`, `attempts` | 一次性配对码，10 分钟过期，5 次错误作废 |
| `parent_accounts` | `id`, `username`, `password_hash`（argon2id/scrypt）, `created_at` | 单家庭默认 1 个管理员（§13 D11） |
| `parent_sessions` | `session_sha`, `account_id`, `expires_at`, `csrf_sha` | HttpOnly+Secure+SameSite=Strict cookie |
| `device_state` | `device_id`, `generation`, `state_sha`, `received_at`, `pet_json`, `battery_pct`, `charging`, `rtc_day`, `inventory_json` | 设备上报的**最新**状态快照（§5.2），只保留一行 |
| `memory_current` | `device_id`, `generation`, `memory_json`, `clear_epoch` | 由快照解出的结构化记忆：喜好 `{slot, option}`、成长计数、里程碑位、最近书目、今日/昨日计数 |
| `daily_summaries` | `device_id`, `day`, `feeds`, `greetings`, `books`, `games`, `choices` | 快照里"今天"换日时归档昨天；保留期限见 §8 |
| `profiles` | `device_id`, `rev`, `pet_name`, `temperament`, `voice_pack_id` | 家长初始化（V5-06） |
| `policies` | `device_id`, `rev`, `settings_json`（作息/音量/体感）, `open_chat`, `open_chat_disabled_reason` | 家长策略；`open_chat` 只有服务端有（设备端无此开关前） |
| `commands` | `command_id`（u64，最高位 0 = 外部 id）, `device_id`, `kind`, `payload_json`, `created_at`, `expires_at`, `state`（pending/applied/rejected/expired）, `result_code` | 服务端→设备下发（§5.3） |
| `content_packages` | `package_id`, `kind`（book/soundpack/game）, `content_id`（u16）, `revision`, `sha256`, `header_crc`, `bytes`, `path`, `status`（draft/approved/retired）, `reviewed_by`, `reviewed_at`, `checklist_json` | **审核绑定到 sha256**：文件任何一个字节变化都是新的待审包 |
| `content_pages` | `package_id`, `page`, `text` | 从**同一个已审核 KPB2 文件**里解出的页文本，供页问答用（替代 CLI 单页导入） |
| `like_topics` / `like_options` | `slot`, `option`, `label`, `package_id` | 从 KPB2 旁路清单导入的喜好主题/选项名，让家长看到"最喜欢的动物 = 大象"而不是数字 |
| `audit_log` | `at`, `actor`（parent/cli/device）, `action`, `target`, `result` | 只记操作类型，不记孩子内容 |
| `moderation_stats` | `device_id`, `day`, `blocked_in`, `blocked_out`, `errors` | 只计数，不存文本 |

**不建表**：录音、ASR 文本、模型回复、家长备注里的身份信息（PRD §8.2 的"家长备注"本期不做，见 §13 D9）。

## 5. 设备认证与同步协议

### 5.1 配对与凭证

现有 `enroll` 要求把 token 手工放进设备，不适合家长。新增**屏显配对码**：

1. 家长在面板点"添加宠物" → 服务端生成 `pairing_id`。
2. 设备家长入口（BOOT+拨轮 5 s）选"连接家里的服务器" → 设备用出厂唯一 id 请求 `POST /v1/pairing/start`，屏幕显示 6 位数字码（局刷提示行）。
3. 家长在面板输入该码 → 服务端核对（常量时间比较、5 次错误作废、10 分钟过期）→ 生成 32 字节随机 token，只存哈希。
4. 设备轮询 `POST /v1/pairing/claim`（带同一配对会话）拿到 token，一次性返回，写入 NVS 后回 `POST /v1/pairing/confirm`。

- **新设备开通**：配对成功即为"新设备"，服务端游标为 0；若设备的迁移标记表明存在旧 `kid_bridge` 未发数据，设备拒绝开启投影（沿用 P3_ATOMIC_PET_LEDGER 的约束），服务端也拒绝复用已有 device_id。
- **轮换**：`POST /v1/devices/{id}/token/rotate`（旧 token 鉴权）→ 新 token 与旧 token 共存 24 h，设备确认后旧的作废。家长可在面板"撤销设备"（`status=revoked`，立即 401）。
- token 在 NVS 中的保护（NVS 加密/flash 加密）放到 V7（§13 D12）。

### 5.2 上行：状态快照同步（推荐，替代"记忆进外发队列"）

**为什么不是事件队列**：16 条外发队列离线时会塞满（P3 已登记风险），NVS 空间不允许再扩；记忆本身已经是有界、确定性的 288 字节状态。**同步状态比同步事件简单且天然幂等**。

`PUT /v1/devices/{id}/state`（设备 token）：

```json
{
  "schema_version": "2.0",
  "generation": 1842,
  "ledger_schema": 3,
  "fw_version": "pet-2026.11.04",
  "rtc_day": 20761,
  "pet": {"mode": "HOME", "mood": "HAPPY", "fed_total": 57},
  "memory_b64": "<288 字节 MemoryStore 规范编码>",
  "parent_settings_b64": "<28 字节 ParentSettingsCodec 编码>",
  "parent_settings_rev": 7,
  "battery": {"pct": 64, "charging": false},
  "inventory": [{"kind": "book", "content_id": 17, "revision": 2, "header_crc": "9a0c11e2"}],
  "applied_commands": [{"command_id": 4097, "result": "applied"}]
}
```

- **记忆用规范二进制编码上传**，服务端用 Python 版解码器（与 C++ `MemoryStore` 共享黄金测试向量，§10.2）严格解码：解码失败即拒绝，不做"尽量解析"。这样服务端和设备对记忆含义只有一个定义。
- **幂等**：以 `(device_id, generation)` 为键。
  - `generation` 大于已存 → 接受，更新投影；
  - 相等且 `state_sha` 相同 → `DUPLICATE`，返回原结果；
  - 相等但内容不同 → `GENERATION_CONFLICT`，不更新，记审计并在面板提示（说明设备账本被回滚或损坏）；
  - 小于已存 → `STALE`，不更新（乱序到达的旧快照）。设备 `generation` 倒退且持续（刷回旧固件/账本重建）→ 面板提示家长"重新开通"，不自动覆盖。
- **频率**：开机联网后一次、每次账本提交后去抖 30 s 一次、关机前一次（如果网络在）；离线时什么也不排队——下次联网只发最新快照。
- **每日归档**：快照里 `today.day` 比上次大 → 把上次快照的"今日"写进 `daily_summaries`；设备只保留今天+昨天，所以**离线超过 2 天，中间几天只留成长总数，不留逐日明细**（明确接受的取舍）。
- **现有事件接口保留**（`POST events`），用于将来需要逐条记录的事件；V5/V6 不依赖它，也不需要板上开启 `SetProjection(true)`。

### 5.3 下行：命令通道

快照响应里携带待执行命令（也可 `GET /v1/devices/{id}/commands` 主动拉）：

```json
{"result": "accepted", "server_rev": 31,
 "commands": [{"command_id": 4097, "kind": "MEMORY_CLEAR", "expires_at": 1793600000, "payload": {"clear_epoch": 3}}]}
```

| kind | payload | 设备侧映射 |
|---|---|---|
| `MEMORY_CLEAR` | `clear_epoch` | `RecordMemory(MemoryCleared)`，来源见 §13 D5 |
| `SET_TEMPERAMENT` | `temperament` | `RecordMemory(TemperamentSet)` |
| `SET_PARENT_SETTINGS` | 28 字节编码 + `base_rev` / `new_rev` | `SetParentSettings(id, ...)` |
| `SELECT_VOICE_PACK` | `content_id` | 设备侧新设置（依赖 §11.2） |

- `command_id` 是服务端分配的稳定 u64，**最高位为 0**，正好落在设备"外部 id"空间（`IsExternalEventId`），设备用它作为账本事件 id → 重放窗口天然去重：同一命令重发不会二次执行。
- `expires_at`：过期命令设备不执行并回 `expired`（INTERFACES_AND_DATA §3 关键规则）。设备时钟未知时只执行"幂等且无时间敏感"的命令（清空、性格）。
- 设备在下一次快照的 `applied_commands` 里回执；服务端据此把命令标记为 applied/rejected。

### 5.4 冲突处理

| 数据 | 权威 | 规则 |
|---|---|---|
| 宠物状态、记忆计数、喜好 | 设备 | 服务端只读投影；按 generation 单调接受 |
| 记忆清空 | 家长 | **墓碑 + 纪元**：家长删除时服务端立即删本地副本、`clear_epoch += 1`、下发 `MEMORY_CLEAR`。在设备回执该命令之前，所有上报快照只更新宠物/电量，**记忆部分丢弃**，防止"删了又被旧快照写回"。设备回执后，下一份快照的记忆才被接受 |
| 家长设置（作息/音量/体感） | 两个写入者（设备家长菜单 + 面板） | 带修订号：面板改动以 `base_rev` 下发；设备若在此期间本地也改过（快照里 `parent_settings_rev` 与 `base_rev` 不同且内容不同）→ 服务端按**更保守合并**（音量取小、安静时段取并集、体感开关取"关"）生成新版本再下发，并在面板提示。**禁止 last-write-wins**（INTERFACES_AND_DATA §8）。修订号需要设备端 ParentSettingsCodec 用保留字节存 rev（codec 版本 1→2，§11.2） |
| 名字、声音包选择 | 面板 | 只有面板写；设备只读 |
| 开放问答开关 | 面板 + 熔断 | 只在服务端；熔断关闭后只有家长能重新打开 |
| 内容包 | 服务端目录 | 只做"已审核版本替换"，不合并 |

## 6. 语音：意图、页问答、角色设定、开放问答

### 6.1 一个入口 `POST /v1/voice/{id}/turn`

守卫只把 ASR 文本交给这个接口、把返回的 `tts_text` 交给 `speak_txt`。调用方用**语音服务专用 token**（与设备 token 分开，作用域只到 `/v1/voice/*` 和只读摘要）。

```json
// 请求
{"text": "这一页讲了什么", "turn_id": "c0ffee..."}
// 响应
{"kind": "approved_content|fixed|local_command|open_chat|fallback",
 "tts_text": "这是一只小象。", "action": null, "reason": "page_qa"}
```

处理顺序（每一步失败都落到固定兜底句，永不回落到 LLM）：

```text
ASR 文本 → 长度/字符检查 → 安静时段? → 固定命令表 → 页问答 → [开放问答开? 且未熔断?]
                                                           ├─ 否 → 固定兜底
                                                           └─ 是 → 输入审核 → LLM(角色设定+最小摘要+当前页) → 输出审核 → TTS
```

### 6.2 固定命令（V6-02）

| 说法（归一化后精确/白名单同义词匹配） | 响应 | 数据来源 |
|---|---|---|
| 团团睡觉 / 我们睡觉吧 | `local_command: SLEEP_REQUEST` + "好的，我们准备睡觉啦。" | 设备执行（可取消的睡觉流程） |
| 几点了 / 现在几点 | "现在是{h}点{m}分。" | 服务端时钟 + 家长配置的时区；模板只有数字槽位 |
| 还有多少电 | "还有百分之{n}的电。"；快照超过 10 分钟 → "看看屏幕右上角的电池吧。" | 最近一次 `device_state.battery` |
| 下一页 / 翻页 | `local_command: PAGE_NEXT` | 设备执行，最后一页由设备判断 |
| 讲故事 / 读绘本 | `local_command: OPEN_READER` | 设备打开阅读器（最近一本） |
| 喂宠物 | "我们用拨轮一起喂团团吧。" | **不通过语音发奖励**（§13 D7） |

**动作怎么到设备**：守卫不能直接改宠物状态。推荐用小智固件 v2 自带的**设备端 MCP 工具**注册一个固定枚举工具 `self.pet.request(action)`（只接受 `SLEEP_REQUEST/PAGE_NEXT/PAGE_PREV/OPEN_READER`），由守卫在 `local_command` 时调用；设备收到后以 `Voice` 来源投进 PetCore，由 PetCore 按当前场景决定执行与否。这不违反 ADR-008：MCP 只承载"提议"，奖励和记忆仍不经过它。调用路径需在锁定的小智版本上核对；核对前 `local_command` 继续回"用按键完成"。

### 6.3 绘本当前页问答（V6-03）

- **上下文来源换成设备**：设备在 BOOT 开始说话时 `PUT /v1/devices/{id}/context {"scene":"READER","content_id":17,"revision":2,"page":3}`（依赖 §11.2）。服务端只在内存里保留 120 s，不落盘。
- 服务端用 `(content_id, revision)` 找**已审核且包头 CRC 与设备清单一致**的包（KPB2/KPG1 的 `header_crc` 覆盖 `tables_crc`，后者覆盖每个数据段的 CRC，所以设备不必重读整包就能报出能区分内容的指纹；它防误配不防篡改，审核权威仍是服务端存的 sha256。KPS1 没有 content_id，按 revision + 包头 CRC 匹配，S4 核对），取 `content_pages` 中第 `page` 页文本。找不到 → 固定句"我们先翻到书里再问吧"。
- 开放问答关闭：只回该页审核原文（现有行为）或该页已审核的题目播报。
- 开放问答开启：LLM 只拿到这一页审核文本作为上下文（不带整本、不带其他页），回答仍走 §6.5 的输出审核。

### 6.4 个性作为角色设定（V6-04）

`persona.py` 只用**结构化参数拼固定模板**，不接受家长自由文本进入系统提示：

- 模板库：每种性格一段审核过的角色说明（如温柔型"说话慢、先安慰再回答、每句不超过 20 个字"），版本化存在代码里，改动走代码审查。
- 槽位：`pet_name`（家长设置，校验：1–6 个汉字，或 1–12 个字母，无数字/符号/链接）、`temperament`、最小记忆摘要（§7.3）、当前页文本（如有）。
- 固定安全前缀：面向 3–5 岁、不讨论危险/恐怖/成人话题、不索要个人信息、不说"保密"、不承诺陪伴排他、不知道就说"问问爸爸妈妈"。
- **输出审核不因角色设定放松**；角色设定只影响语气。

### 6.5 开放问答开关与审核失败关闭（V6-05）

- **开关**：`policies.open_chat` 默认 `false`。只有家长会话可以打开；打开时面板显示风险说明并要求再次输入密码。
- **审核端口** `moderation.py`：`ModerationPort.check_input(text)` / `check_output(text)` → `allow | block | error`。提供方可插拔（§13 D8）；测试用确定性假实现。外加本地硬规则（长度、电话/地址/链接模式、敏感词表）**先于**外部审核执行，硬规则不通过直接 block。
- **整句审核后再发声**：LLM 回复先完整缓冲（限 120 字），通过输出审核才进 TTS；不做流式边说边审（PRD/INTERFACES §7）。若以后要流式，必须按句缓冲、逐句审核，单列评审。
- **失败关闭（熔断）**：
  - 任一步 `error`/超时（审核 2 s、LLM 6 s）→ 本轮播固定句；
  - 10 分钟内连续 3 次 `error`，或一天内输出被 `block` ≥ 5 次 → 自动 `open_chat=false`、`open_chat_disabled_reason` 记原因、审计记录；**只有家长手动才能重新打开**；
  - 审核提供方配置缺失 → 开关无法打开（面板置灰）。
- **TTS 出口审计**：S6 必须列出锁定版本小智服务端所有 TTS 来源（欢迎语、报错、告别、超时提示、工具回调播报），儿童模式下逐一替换为固定句或拦截，并加 overlay 合约测试。未完成前，开放问答在任何设备上都不能打开（代码级开关）。

## 7. 记忆、初始化与内容的 API

### 7.1 接口总表

鉴权：**D** = 设备 token；**V** = 语音服务 token；**P** = 家长会话（+CSRF）；**C** = 仅本机 CLI。

| 方法 / 路由 | 鉴权 | 任务 | 说明 |
|---|---|---|---|
| `POST /v1/pairing/start`、`/claim`、`/confirm` | 配对码 | S1 | §5.1 |
| `POST /v1/devices/{id}/token/rotate` | D | S1 | |
| `PUT /v1/devices/{id}/state` | D | S2 / V5-05 | §5.2，响应带命令 |
| `GET /v1/devices/{id}/commands` | D | S2 | |
| `PUT /v1/devices/{id}/context` | D | S5 / V6-03 | 内存 120 s |
| `GET /v1/content/manifest` | D | S4 | 已审核包：kind、content_id、revision、sha256、bytes |
| `GET /v1/content/packages/{sha256}` | D | S4 | 支持 `Range`；只给 approved；`ETag = sha256` |
| `POST /v1/voice/{id}/turn` | V | S5/S6 | §6 |
| `GET /v1/voice/{id}/summary` | V | S2 / V5-05 | §7.3 最小摘要 |
| `POST /v1/parent/login`、`/logout` | 密码 | S0 | 限流 + 失败锁定 |
| `GET /v1/parent/devices`；`POST /v1/parent/pairings`；`DELETE /v1/parent/devices/{id}` | P | S1 | 删除设备 = 级联删除其全部数据 |
| `GET /v1/parent/devices/{id}/memories` | P | S2 / V5-05 | 喜好（带选项名）、成长、近 N 天摘要、最近书目 |
| `DELETE /v1/parent/devices/{id}/memories` | P | S2 / V5-05 | 立即删服务端副本 + 下发清空；返回命令状态 |
| `GET/PUT /v1/parent/devices/{id}/profile` | P | S3 / V5-06 | 名字、性格、声音包 |
| `GET/PUT /v1/parent/devices/{id}/policy` | P | S3 | 作息/音量/体感 + 开放问答 |
| `GET /v1/parent/devices/{id}/content` | P | S4 / V6-06 | 设备卡上清单 vs 已审核目录：已审核/未审核/版本过旧 |
| `POST /v1/parent/content`（上传）、`POST .../{pkg}/approve`、`POST .../{pkg}/retire` | P | S4 | 审核绑定 sha256 + 清单勾选 |
| `GET /v1/parent/audit` | P | S0 | |
| `pet_learning_admin create-parent / import-pack / backup / restore` | C | S0/S4/S7 | 本机可信操作 |

家长面板：PetLearning 直接提供**服务端渲染的少量 HTML 页**（无前端构建链，沿用 playground 的 CSP 头），只在局域网。是否做面板见 §13 D3。

### 7.2 家长初始化流程（V5-06）

```text
面板"添加宠物" → 设备屏显配对码 → 输入码 → 设置：名字 / 性格（三选一，配示例句试听）/ 声音包（从设备上报的 TF 卡已审核声音包里选）
→ 服务端写 profiles(rev=1) → 下发 SET_TEMPERAMENT、SELECT_VOICE_PACK → 设备回执 → 面板显示"已生效"
```

- 名字只用于**联网语音的角色设定和面板显示**；设备上的录音话术仍是声音包里录好的称呼（§13 D9）。
- 声音包只能选设备 inventory 里**且**服务端目录中已审核（sha256 对上）的包；KPS1 包头以 `--reviewed` 构建只是格式前提，不等于本服务端审核。

### 7.3 给语音服务的最小摘要（V5-05）

服务端从 `memory_current` 生成，与设备 `MakeMinimalSummary()` 字段一致，再把数字 id 换成**审核过的选项名**：

```json
{"name": "团团", "trait": "gentle",
 "likes": [{"topic": "最喜欢的动物", "option": "大象"}],
 "today": {"feed": 2, "books": 1, "games": 0}, "last_book": "森林里的朋友"}
```

- 不含：事件 id、日期、成长明细、家长账号、设备 id、任何自由文本。
- 选项名只取自 `like_options`（来自已审核包）；没有名字的 id 直接省略，不把数字交给模型。
- 记忆清空后、设备回执前，摘要只返回 `name` 和 `trait`。

### 7.4 内容与声音包分发

- **导入**：`import-pack`（CLI）或面板上传 → 按魔数分派到 `tools/kpb2.py` / `tools/sound_pack.py` / `tools/game_pack.py` 的**现有校验器**（不另写一套解析）→ 存 `data/packages/<sha256>` → `draft`。
- **审核**：家长在面板逐项勾 [V4_CONTENT_REVIEW_CHECKLIST](V4_CONTENT_REVIEW_CHECKLIST.md)（预览图由 `story_pack preview` 生成）→ `approve` 记录审核人、时间、sha256、清单。改任何字节 = 新包，重新审核；同 `content_id` 的新 revision 审核通过后旧版本自动 `retired`。
- **派生数据**：审核通过时从同一文件解出页文本 → `content_pages`，旁路清单的喜好主题 → `like_topics/options`。CLI `import-page` 保留给测试，标注为旧路径。
- **分发**：本期只提供 manifest + 下载接口（带 Range、sha256 校验），设备端"联网下载写 TF 卡"是否在 V6 做见 §13 D10；默认仍由家长拷卡，服务端用设备 inventory 告诉家长卡上哪些包"未审核/已过期"。
- **未审核内容一律不可见**：manifest、下载、页问答、摘要都只读 `approved`。

## 8. 儿童隐私

| 项 | 规则 |
|---|---|
| 录音 | PetLearning 从不接收音频；小智服务端关闭音频落盘（部署检查清单逐项核对，S7） |
| ASR 文本 | 只在单次请求内存中；不写库、不写日志（日志只记 `kind/reason/耗时`） |
| 模型回复 | 不存；只存审核计数 |
| 记忆 | 只有结构化数字 id + 计数；最新一份 + 每日摘要 |
| 家长输入 | 名字（受校验）；不收集生日、住址、学校、照片 |
| 第三方 | 开放问答打开时，**只**把 ASR 文本、角色设定、最小摘要、当前页文本发给 LLM/审核提供方；面板写明发给了谁 |
| 删除 | 家长"清空记忆"（§5.4 墓碑）；"删除设备" = 删除该设备全部行（事件回执、快照、摘要、命令、审核计数）+ 撤销 token；"导出"给家长一份 JSON |
| 保留期限（默认，§13 D6） | 每日摘要 90 天；审计日志 365 天；`commands` 已完成 30 天；`moderation_stats` 90 天；应用日志 14 天；配对码 10 分钟；语音上下文 120 s（仅内存）。每日清理任务 + 测试 |
| 备份 | 备份里同样会有记忆：备份文件加密，删除设备时提示家长旧备份仍含数据 |

## 9. 安全

- **传输**：设备与家长都只走 HTTPS（Caddy 终止 TLS）；PetLearning 和小智服务端只监听回环。设备端 `EspPetHttpsTransport` 已强制证书校验、拒绝重定向。证书方案见 §13 D2：推荐"家长自有域名 + DNS-01 签发公信证书，域名解析到局域网 IP"，固件用现有证书包零改动；备选自签 CA 打进固件（要改固件、换证书要 OTA）。
- **三种身份互不通用**：设备 token（只能访问自己的 `/v1/devices/{id}/*`、manifest、下载）、语音 token（只能 `/v1/voice/*`）、家长会话（只能 `/v1/parent/*`）。每个路由的鉴权在路由表上声明并有测试"其他身份 → 401/403"。
- **家长认证**：密码 argon2id（或 hashlib.scrypt，免新依赖，§13 D11 一并定）；登录限流 5 次/15 分钟；会话 12 h；敏感操作（打开开放问答、删除设备）要求再输密码；CSRF 双提交令牌。
- **限流**：Caddy 按 IP；应用内按 device_id（状态上报 ≥10 s 间隔、语音 1 次/s）。
- **密钥管理**：所有密钥（家长初始密码、语音 token、外部模型/审核 API key）只放在 `.env`/Docker secrets，仓库只提交 `.env.example`；启动时检查必需密钥长度并拒绝弱值；日志与错误信息绝不含 token；现有 "token 不经命令行参数" 规则保留。
- **输入校验**：沿用 `StrictModel(extra="forbid")`；请求体上限（状态 4 KiB、上传包 64 MiB）；所有 id 用正则白名单。
- **上传包**：只按魔数分派到现有解析器，存储文件名为 sha256，不使用上传文件名，下载 `Content-Disposition` 固定。
- **供应链**：`requirements.txt` 加上限并锁定哈希（`pip-compile --generate-hashes`）；新增依赖仅限必要（审核/LLM SDK 用 `httpx` 直接调，避免大 SDK）。

## 10. 测试策略

沿用 `unittest` + FastAPI `TestClient`，不引入 pytest；全部在 CI 现有 Python 任务中运行：`python -m unittest discover -s tests -p "test_*.py"`。

| 层 | 文件（建议） | 内容 |
|---|---|---|
| 迁移 | `test_v5_server_migrations.py` | 空库、P3 旧库（用现有建表生成）→ 新版；版本高于代码拒绝启动 |
| 编码契约 | `test_v5_memory_codec.py` + `tests/fixtures/memory/*.bin` | Python 解码器与 C++ 共享黄金向量：C++ 测试写出、Python 测试读入（或反之），覆盖 V5 文档列出的 12 种损坏；ParentSettings 28 字节同理 |
| 同步 | `test_v5_state_sync.py` | 相同 generation 重放=DUPLICATE；同 generation 不同内容=冲突；乱序旧快照=STALE；换日归档；离线 3 天只留成长；**清空墓碑**：删除后旧快照不写回记忆、回执后恢复接受；命令过期；命令 id 最高位为 0 |
| 设置合并 | `test_v5_policy_merge.py` | 双方改动的保守合并表驱动测试 |
| 配对/鉴权 | `test_v5_pairing_auth.py` | 配对码过期/错 5 次作废；token 轮换宽限；三种身份越权全部拒绝；撤销后 401；CSRF |
| 意图 | `test_v6_voice_turn.py` | 每条固定命令；电量过期兜底；安静时段；页问答只返回包头 CRC 与设备清单一致的已审核页；未知话语兜底；ASR 文本不出现在日志（捕获日志断言） |
| 开放问答 | `test_v6_open_chat_safety.py` | 假审核/假 LLM：输入 block、输出 block、超时、异常、配置缺失 → 都是固定句；熔断阈值；熔断后只有家长能开；TTS 只出现审核通过的文本；红队语料（固定列表：索要住址、危险模仿、恐怖、成人话题、"别告诉爸妈"）全部不出声 |
| 内容 | `test_v6_content_catalog.py` | 用 `tests/fixtures/kpb2/golden_book.kpb` 等现有夹具导入；改 1 字节 = 新草稿；未审核不进 manifest/下载/页问答；Range 下载与 sha256 |
| 隐私 | `test_v5_retention.py` | 清理任务按期限删除；删除设备级联；导出内容不含 token |
| 守卫 | 扩展 `test_p3_xiaozhi_child_voice_guard.py` | 改为调 `/v1/voice/turn`；`local_command` 的 MCP 提议仍不改宠物状态；TTS 出口审计合约 |

不做：真机、真实 LLM/审核 API（只有手动冒烟脚本，需家长陪同）。

## 11. 分阶段里程碑

### 11.1 服务端阶段（每个阶段一个可独立合并的 PR）

```text
S0 基础 ──┬─> S1 配对与凭证
          ├─> S2 状态同步+记忆（V5-05） ──> S3 初始化+策略（V5-06）──┐
          ├─> S4 内容目录与分发（V6-06 服务端） ──> S5 语音意图+页问答（V6-02/03）──> S6 开放问答（V6-04/05）
          └─> S7 部署与运维（任何时候）
```

| 阶段 | 内容 | 主要文件 | 依赖 | 验收 |
|---|---|---|---|---|
| **S0 基础** | 迁移框架（`user_version`）、路由拆分（device/voice/parent 三个 router）、配置模块、家长账号（CLI 创建）与会话、审计日志 | `db.py`、`migrations/`、`auth.py`、`routers/` | — | 现有 P3 测试全绿 + 迁移/会话测试 |
| **S1 配对与凭证** | 配对码、token 轮换/撤销、限流 | `pairing.py` | S0 | 配对/鉴权测试 |
| **S2 状态同步与记忆（V5-05）** | `PUT state`、Python 记忆/设置解码器 + 黄金向量、记忆投影、每日归档、命令通道、清空墓碑、最小摘要、家长查看/删除 | `sync.py`、`memory_codec.py`、`commands.py` | S0（可先用 CLI enroll 的 token，与 S1 并行） | 同步/墓碑/契约测试 |
| **S3 初始化与策略（V5-06）** | profile/policy 接口、性格/声音包/设置命令、保守合并 | `profile.py`、`policy.py` | S2（命令通道） | 合并测试 + API 测试 |
| **S4 内容目录与分发** | 导入（复用 tools 校验器）、sha256 绑定审核、页文本/喜好名派生、manifest/下载、设备清单比对 | `content.py` | S0（与 S1/S2 并行） | 内容测试 |
| **S5 语音意图与页问答（V6-02/03）** | `/v1/voice/turn`、新固定命令、`context` 接口、页问答改用 `content_pages`、守卫改调新接口 | `voice.py`、`intent.py`、守卫 | S4（页文本）；电量命令用 S2 的快照，S2 未合并时该命令走"看屏幕"兜底 | 意图测试 |
| **S6 开放问答（V6-04/05）** | 角色模板、审核端口 + 硬规则、整句缓冲、熔断、开关、TTS 出口审计 | `persona.py`、`moderation.py`、overlay | S3（名字/性格/开关）+ S5 | 开放问答安全测试；未过审计前代码级禁用 |
| **S7 部署与运维** | Compose + Caddy、`.env.example`、证书说明、备份/恢复、保留期清理任务、小智服务端儿童模式配置检查清单 | `deploy/`、`tools/pet_learning_admin.py` | S0 | 备份恢复测试；文档 |

**可并行的切分**：S0 合并后，S1、S2、S4、S7 四条线互不改同一文件，可同时开发；S3 等 S2，S5 等 S4，S6 最后。建议各阶段只在自己的新模块里加代码，对 `api.py` 只做"挂载 router"一行改动，减少合并冲突。

### 11.2 设备端配套（另开任务，不在本方案实现范围）

| 设备端任务 | 服务端依赖它的阶段 |
|---|---|
| 板上启动 `FreertosPetHttpWorker` 的状态上报版本：组装 §5.2 快照（读 `MemoryNow()` 等副本，不在网络任务里碰账本） | S2 端到端 |
| 命令应用器：网络任务把命令拷贝投进 Pet 任务收件箱，Pet 任务用 `command_id` 作为账本事件 id 提交 | S2/S3 端到端 |
| 家长入口"连接服务器"：屏显配对码、token 写 NVS | S1 端到端 |
| ParentSettingsCodec v2：保留字节存 `rev` | S3 合并规则 |
| 多声音包选择 | S3 声音包 |
| BOOT 说话时上报 `context`；设备端 MCP 工具 `self.pet.request` | S5 |
| TF 卡联网下载（若 D10 选"做"） | S4 下载 |

## 12. 风险

| 风险 | 后果 | 预案 |
|---|---|---|
| 设备端配套迟迟不做 | 服务端只能用模拟设备测，V5-05/V6 无法真机验收 | 先合并服务端（契约测试锁住协议），设备端按 §11.2 逐项排期；提供 `tools/fake_device.py` 模拟上报 |
| 局域网 TLS 证书 | 设备连不上或被迫关校验 | 绝不关校验；D2 选定方案后在 S7 写清续期 |
| 小智上游其他 TTS 出口 | 未审核文本发声 | S6 出口审计未完成 → 开放问答代码级禁用 |
| 外部审核误放 | 不当内容 | 本地硬规则先行 + 输出审核 + 熔断 + 家长陪同前提；红队语料持续扩充 |
| 记忆解码器与 C++ 漂移 | 家长看到错误记忆 | 共享黄金向量；记忆 schema 变化必须同时改两边 |
| 离线 >2 天丢逐日明细 | 家长看到的历史有空缺 | 文档告知；成长总数不丢 |
| generation 倒退（刷旧固件） | 投影与设备不一致 | 不自动覆盖，面板提示重新开通 |
| SQLite 单写者 | 并发高时阻塞 | 单家庭量级足够；所有写事务短小 |
| 家长密码泄露 | 局域网内他人改设置 | 仅局域网、限流、敏感操作二次确认、审计 |

## 13. 待用户拍板的决策

| # | 问题 | 选项 | 推荐 |
|---|---|---|---|
| D1 | 服务端跑在哪台机器、用什么形态 | 本机 Windows（数据放 F 盘）/ NAS / 树莓派；Docker Compose / 裸跑 | 常开的一台局域网主机 + Docker Compose；数据目录放非系统盘 |
| D2 | 局域网 HTTPS 证书 | A. 自有域名 + DNS-01 公信证书（固件零改动）；B. 自签 CA 打进固件 | A |
| D3 | 家长端形态与访问范围 | 局域网网页面板 / 先只做 CLI；是否需要外网访问 | 局域网网页面板，不开外网 |
| D4 | 记忆/状态同步方式 | 状态快照（§5.2）/ 扩外发事件队列 | 状态快照 |
| D5 | 面板下发的"清空记忆/改性格"在设备上要不要家长再确认 | 直接执行（家长已在面板登录）/ 设备家长入口里再按一次确认 | 直接执行，设备上显示"家长清空了记忆"提示 |
| D6 | 保留期限 | §8 默认值，或更短/更长 | 每日摘要 90 天、审计 365 天 |
| D7 | 语音能否触发动作 | 只允许睡觉/翻页/开书（无奖励类）；喂食等奖励类不允许 | 如左；动作通过设备端 MCP 固定枚举工具提议 |
| D8 | 开放问答的 LLM 与审核提供方 | 国内云内容安全 API / 其他云审核 / 本地模型判定；LLM 用哪家 | 需用户指定提供方和预算；未定之前 S6 只交付假实现和代码级禁用 |
| D9 | 宠物名字的作用范围；是否做"家长备注" | 名字只用于联网语音和面板 / 也要设备端发声（需录音或 TTS 缓存）；家长备注做/不做 | 名字只用于联网语音和面板；家长备注本期不做 |
| D10 | 内容包是否联网下载到 TF 卡 | V6 就做（设备端工作量大）/ 继续家长拷卡，服务端只做目录与比对 | 本期继续拷卡，服务端先做目录、清单比对和下载接口 |
| D11 | 账号模型与密码哈希 | 单家庭单管理员 / 多家长账号；argon2id（新依赖）/ scrypt（标准库） | 单管理员 + scrypt |
| D12 | 设备 token 在 NVS 中的保护 | 现在就上 NVS 加密 / 留到 V7 与 flash 加密一起 | 留到 V7，先限定局域网 + 可撤销 |

## 14. 相关文档

[开发计划](DEVELOPMENT_PLAN.md) · [技术架构](ARCHITECTURE.md) · [接口与数据](INTERFACES_AND_DATA.md) · [ADR](DECISIONS.md) · [原子账本](P3_ATOMIC_PET_LEDGER.md) · [个性与记忆](V5_PERSONALITY_MEMORY.md) · [ESP HTTPS 同步](P3_ESP_HTTPS_SYNC.md) · [小智语音桥](P3_XIAOZHI_VOICE_BRIDGE.md) · [KPB2 格式](V4_KPB2_CONTENT_FORMAT.md) · [声音包](V2_SOUND_PACK.md) · [内容审核清单](V4_CONTENT_REVIEW_CHECKLIST.md)
