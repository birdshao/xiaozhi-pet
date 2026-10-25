# 接口、状态与资源包规范（草案）

状态：v0.1 / 待 P2–P4 实现与契约测试。示例仅定义**计划中的自有业务协议**，不代表小智官方已有这些接口。

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

## 3. 设备事件（候选）

| event | 来源 | 是否离线执行 | 说明 |
|---|---|---|---|
| PET_PICKUP | IMU | 是 | 去抖后唤起宠物表情和本地音效 |
| PET_FEED | 按键/菜单 | 是 | 使用事先允许的 item_id，幂等更新 |
| PET_PLAY | 按键/菜单 | 是 | 进入指定本地小游戏 |
| STORY_OPEN | 内容播放器 | 是 | 书籍/页码来自已审核资源 |
| PAGE_NEXT / PAGE_PREV | 输入 | 是 | 页码有边界检查 |
| VOICE_STARTED / VOICE_ENDED | 语音适配层 | 是 | 影响显示和音频优先级 |
| GAME_COMPLETED | 游戏逻辑 | 是 | 只能由可信游戏规则产生，不能由 LLM 伪造 |
| NETWORK_RESTORED | 设备服务 | 是 | 触发可靠同步 |
| QUIET_HOURS_START | 策略计时器 | 是 | 本地保护规则优先 |

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

安全要求：device token 只可访问对应设备；voice adapter 只拿最小上下文；parent token 需要独立认证。API 不能直接裸露公网且无鉴权，开发初期可以只在可信 LAN 内运行。

## 5. 宠物本地数据模型（候选）

```json
{
  "schema_version": 1,
  "pet_id": "pet-a1",
  "name": "团团",
  "pet_state_version": 18,
  "level": 2,
  "cosmetics": ["hat_01"],
  "current_scene": "HOME",
  "mood": "HAPPY",
  "daily_interaction_count": 3,
  "active_book_id": "animals_001",
  "active_page": 2,
  "parent_policy_version": 5,
  "last_acked_seq": 1008
}
```

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

画面格式 `.bin`、语音 `.opus` 是候选；P4 根据实际播放器/解码器选择（可替换为 PCM/WAV 或其他具备稳定硬解码/软解码路径的格式）。对传入素材应限制尺寸、时长和解码后缓冲预算；未标记 `approved` 的内容不得同步给儿童设备。

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
