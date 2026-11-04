# 接口、状态与资源包规范（草案）

状态：v0.2（2026-11-02 按实物硬件更新 §3、§5、§6）/ 待 V2–V5 实现与契约测试。示例仅定义**计划中的自有业务协议**，不代表小智官方已有这些接口。

## 1. 通用边界

- **小智设备语音协议**：交给原开源固件和自建语音服务端维护；不要扩展其二进制音频包来夹带业务私有字段，除非有充分证据。
- **Pet Business Protocol**：单独版本化，设备通过 DeviceBridge 与自己的 Python 服务通信。
- **模型工具**：只暴露经过批准的 `get_page_context`、`get_pet_summary`、`propose_game` 等；模型不能直接指定任意积分、电量或画面像素。
- 所有写操作都验证设备身份、授权、作用域，带幂等键；未授权的孩子不能更改家长策略。

## 2. 标准消息信封

```json
{
  "schema_version": "1.0",
  "message_id": "dev-a1:00000042",
  "device_id": "dev-a1",
  "session_id": "session-20261025-01",
  "seq": 42,
  "sent_at_ms": 1793000000000,
  "type": "pet.event",
  "payload": {
    "event": "PET_FEED",
    "item_id": "carrot"
  }
}
```

- `message_id` 在源设备持久唯一，断线重传不可变；`seq` 在设备端单调递增（重启延续）。
- `sent_at_ms` 是示例字段，时间戳不应单独用于排序，避免 RTC 未校准；排序以 `seq`、`state_version` 为准。
- `session_id` 是传输会话标识，不作为事件唯一键。
- 超出版本范围、身份不符、字段缺失、超过大小上限的消息应被拒绝并记录非敏感错误码。
- 返回 `ack` 包括 `message_id`、`accepted`、`server_state_version` 和失败 `reason_code`。
- 网络重试采用有界指数退避；客户端保存有限事件队列并避免无限写 Flash。

## 3. 设备事件（v0.2，按实物硬件）

### 3.1 本地输入与传感事件（不出设备，只进 PetCore）

| InputEvent / SensorEvent | 来源 | 说明 |
|---|---|---|
| `DIAL_UP` / `DIAL_DOWN` | 拨轮 GPIO6 / GPIO4（真机核对） | 换选项 / 翻页；列表中按住自动重复 ≤3 次/秒 |
| `DIAL_PRESS` / `DIAL_LONG` | 拨轮 GPIO5 | 确认 / 回首页（≥1 s） |
| `TALK` | BOOT GPIO0 | 开始/结束语音会话 |
| `SLEEP_REQUEST` | PWR GPIO1 | 进入睡觉流程，3 s 内拨轮可取消 |
| `PARENT_COMBO` | BOOT + 拨轮按下 5 s | 家长入口 |
| `PICKUP` / `GENTLE_SHAKE` / `HARD_SHAKE` / `FACE_DOWN` / `FACE_UP` | QMI8658 | 去抖 + 持续时间判定 |
| `TILT(x,y)` | QMI8658 | 仅游戏中按帧提供 |
| `ENV_CHANGED(temp_c10, hum_pct)` | SHTC3 | 越过阈值才发 |
| `BLOW` / `CLAP` | 麦克风 RMS 包络 | 仅游戏中；不缓存 PCM |
| `BATTERY(pct, charging)` | TG28 | 变化 ≥5% 或充电状态变化 |
| `DAY_PHASE(phase)` / `QUIET_BEGIN` / `QUIET_END` | RhythmScheduler（RTC） | 作息相位变化 |
| `WAKE(reason)` | PowerManager | `button` / `rtc_alarm` / `usb` / `motion` |

### 3.2 可上报的宠物事件（进入账本和外发队列）

| event | 触发 | 是否离线执行 | 说明 |
|---|---|---|---|
| PET_FEED / PET_GROOM / PET_DRESS | 动作条/互动页 | 是 | item_id 来自白名单，幂等 |
| PET_GREETED | `PICKUP` / 早安开机 | 是 | 每日计数有上限，不刷奖励 |
| STORY_OPEN / PAGE_NEXT / PAGE_PREV / STORY_FINISHED | 绘本 | 是 | 页码有边界检查 |
| QUIZ_ANSWERED | 页内题/游戏 | 是 | 记录选项 ID；不对"答错"惩罚 |
| PREFERENCE_CHOSEN | 喜好选择题 | 是 | 唯一写喜好记忆的路径 |
| GAME_COMPLETED | 规则/体感游戏 | 是 | 只能由可信游戏规则产生，不能由 LLM 伪造 |
| PET_SLEPT / PET_WOKE | 睡觉流程 / 唤醒 | 是 | 附唤醒原因 |
| MEMORY_CLEARED | 家长 | 是 | 账本事务内清空 |
| VOICE_STARTED / VOICE_ENDED | 语音适配层 | 是 | 影响显示和音频优先级 |
| NETWORK_RESTORED | 设备服务 | 是 | 触发可靠同步 |

### 关键规则

- 对事件处理实行“先校验→执行→持久化/入队→确认”的确定性顺序。
- 对重复 `message_id` 返回相同处理结果，不重复增加成长积分。
- 对来自服务器的命令 `expires_at` 做超时检查；旧的“说一句话/切页”不应该在数分钟后突然执行。
- 音频状态、场景状态和宠物数值分离；场景渲染失败不能回滚已提交的喂食事件。

## 4. Python HTTP API（草案）

| 方法 / 路由 | 使用方 | 功能 | 返回 |
|---|---|---|---|
| `POST /v1/devices/{id}/events` | ESP32 bridge | 批量提交可靠事件 | per-event ACK / state_version |
| `GET /v1/devices/{id}/snapshot` | ESP32 bridge | 获取宠物快照与策略版本 | `PetSnapshot` |
| `GET /v1/pets/{id}/summary` | VoiceAdapter | 只读最小化宠物摘要 | 允许字段 |
| `POST /v1/pets/{id}/actions` | 授权业务层 | 执行喂食/玩耍等动作 | action_result |
| `GET /v1/books/{id}/pages/{page}` | VoiceAdapter | 当前页的已审核故事上下文 | text, approved_questions |
| `GET /v1/content/manifest` | 设备/家长端 | 返回已批准内容包清单 | ID/版本/hash |
| `GET /v1/content/packages/{id}` | 设备端 | 下载已批准打包资源 | 二进制内容 |
| `GET/PUT /v1/parent/policy` | 家长面板 | 时长、网络、聊天与内容白名单 | policy/version |
| `DELETE /v1/parent/memories` | 家长面板 | 删除允许删除的宠物偏好/历史 | 确认回执 |
| `GET /v1/parent/memories` | 家长面板 | 查看喜好、成长、每日摘要 | 记忆列表 |
| `GET /v1/pets/{id}/memory-summary` | VoiceAdapter | 最小化记忆摘要（§5.1） | 允许字段 |
| `GET/PUT /v1/parent/rhythm` | 家长面板 | 作息表、安静时段、音量上限 | policy/version |
| `PUT /v1/parent/personality` | 家长面板 | 名字、性格、声音包 | version |

安全要求：device token 只可访问对应设备；voice adapter 只拿最小上下文；parent token 需要独立认证。API 不能直接裸露公网且无鉴权，开发初期可以只在可信 LAN 内运行。

## 5. 宠物本地数据模型（v2 候选）

```json
{
  "schema_version": 2,
  "pet_id": "pet-a1",
  "name": "团团",
  "personality": { "trait": "curious", "voice_pack": "tuantuan_v1", "catchphrase_id": 3 },
  "pet_state_version": 18,
  "scene": "HOME",
  "home_selection": 1,
  "mood": "HAPPY",
  "cosmetics": ["scarf_01"],
  "active_book_id": "animals_001",
  "active_page": 2,
  "rhythm": { "wake": "07:00", "evening": "19:30", "quiet": ["20:30", "07:00"], "policy_version": 5 },
  "limits": { "volume_max": 60, "night_volume_max": 30, "open_chat": false },
  "last_acked_seq": 1008
}
```

### 5.1 记忆（v2 新增，进入同一账本事务）

```json
{
  "today": { "date": "2026-11-02", "feed": 2, "books_read": 1, "games": 3, "greetings": 4 },
  "preferences": [
    { "slot": "favorite_animal", "option_id": "elephant", "source_event": "dev-a1:00000301" },
    { "slot": "favorite_color", "option_id": "blue", "source_event": "dev-a1:00000302" }
  ],
  "milestones": ["first_book", "five_books"],
  "recent_books": ["animals_001", "forest_friends"]
}
```

- 设备端实现（V5-01..03）：`MemoryStore` 编码在 `PetAtomicLedger` schema v2 里，与宠物状态和外发队列同一次提交；v1 存档可读并迁移。字段、上限、写入守卫和最小摘要格式见 [V5_PERSONALITY_MEMORY.md](V5_PERSONALITY_MEMORY.md)。设备端用数字 id 表示 slot/option/书目，`today.date` 为本地日序号。
- `preferences` 最多 16 条，`slot` 与 `option_id` 都来自内容包白名单；只能由 `PREFERENCE_CHOSEN` 写入，必须带来源事件。
- `today` 每日归档为计数摘要后上报，设备端只保留当天。
- 给语音服务的摘要只含 `name`、`personality.trait`、`preferences` 的选项名、`today` 计数；不含家长备注。

**禁止**长期存储原始孩子语音、详细地址、不必要的生日、秘密或敏感身份属性。昵称、喜好等均由家长审核允许，具备删除机制。

长期状态服务端可以有更多审计元数据，但客户端必须可以在网络断开时正常运行。等级/道具算法固定且有迁移测试。

## 6. 内容包规范（候选）

```text
animals_001/
  manifest.json
  images/page_001.bin
  images/page_002.bin
  audio/page_001.opus
  audio/page_002.opus
  quiz/questions.json
```

```json
{
  "schema_version": "1.0",
  "book_id": "animals_001",
  "version": 1,
  "title": "动物朋友",
  "age_min": 3,
  "age_max": 5,
  "language": "zh-CN",
  "approval_status": "approved",
  "pages": [
    {
      "page": 1,
      "image": "images/page_001.bin",
      "audio": "audio/page_001.opus",
      "text": "这是一只大象。",
      "approved_question_ids": ["q01"]
    }
  ],
  "sha256": "<package payload SHA256>"
}
```

`sha256` 位置仅是格式示意，最终应定义究竟对哪个规范化字节序列计算哈希，避免将 manifest 中自己的 hash 再参与哈希产生循环依赖。计划采用单独的包索引文件记录每个文件哈希与整体打包产物哈希。

**已落地格式**：P4 实现了单文件 KPB1（1-bit 页 + 24 kHz PCM16），见 [P4_OFFLINE_STORY_PACK](P4_OFFLINE_STORY_PACK.md)。v0.2 计划扩展为 **KPB2**：页帧可为 1-bit（48,000 B）或 4 灰度（96,000 B）；每页可带 0–2 个选择题（题干音频、2–3 个选项的图标区域坐标与选项音频、正确项或"无对错"）；包类型 `book | game | soundpack | costume | phrases`；KPB1 继续可读。

画面格式 `.bin`、语音 `.opus` 是早期候选；P4 根据实际播放器/解码器选择（可替换为 PCM/WAV 或其他具备稳定硬解码/软解码路径的格式）。对传入素材应限制尺寸、时长和解码后缓冲预算；未标记 `approved` 的内容不得同步给儿童设备。

## 7. VoiceAdapter 接口（规划）

```python
from dataclasses import dataclass

@dataclass(frozen=True)
class DialogueContext:
    pet_id: str
    scene: str
    book_id: str | None
    page: int | None
    allowed_age_band: str = "3-5"

class ChildDialoguePort:
    async def get_context(self, device_id: str) -> DialogueContext: ...
    async def route_intent(self, text: str, context: DialogueContext): ...
    async def evaluate_input(self, text: str): ...
    async def evaluate_output(self, text: str): ...
    async def emit_pet_intent(self, device_id: str, intent: str, args: dict): ...
```

关键约束：未通过 `evaluate_output` 的文本永远不能进入 TTS 播放队列；对流式回复需要先缓存片段并审核后放行，且流式安全不是“事后删掉字幕”。

## 8. 数据迁移和一致性

- 每个 SQLite schema、客户端状态结构、内容包都有自己的版本号。
- 断网：本地执行确定性宠物命令并持久化待同步事件。
- 重连：上送事件列表→服务端幂等处理→返回权威版本→本地 ACK 并删除已确认队列。
- 同步冲突：宠物养成事件归并；家长策略以最新有效签名/版本为准；资源包只做批准版本替换。
- 回滚：新固件必须可读取或安全迁移旧版存档；升级失败保留旧存档。
- 禁止默认通过 `last-write-wins` 覆盖家长规则或产生重复道具。

## 9. 后续细化顺序

P2 固定事件枚举及状态迁移；P3 固定 voice adapter 的真实入口；P4 固定内容格式及资源打包；P5 固定 OTA/data migration。每次 schema 变化伴随契约测试更新。
