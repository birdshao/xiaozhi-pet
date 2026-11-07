# 服务端 S2：状态同步与记忆（V5-05）

实现范围对应 `V5_V6_SERVER_PLAN.md` §5.2–§5.4、§7.1/§7.3、§8、§11.1 S2。代码：`services/pet_learning/sync.py`（快照、投影、墓碑、家长/语音接口、保留期）、`commands.py`（命令队列）、`memory_codec.py`（与 C++ 共用黄金向量的解码器）、`migrations/005_state_sync.sql`。`api.py` 只加一行挂载。设备端配套（§11.2）不在本阶段。

## 接口

| 方法 / 路由 | 鉴权 | 说明 |
|---|---|---|
| `PUT /v1/devices/{id}/state` | 设备 token（只对路径里的 id 有效） | 快照；响应带待执行命令 |
| `GET /v1/devices/{id}/commands` | 设备 token | 主动拉待执行命令 |
| `GET /v1/parent/devices/{id}/memories?days=30` | 家长会话 | 喜好（带审核名）、成长、今日、最近书目、近 N 天摘要、清空状态、告警 |
| `DELETE /v1/parent/devices/{id}/memories` | 家长会话 + CSRF | 立即删服务端副本（含每日摘要）+ 下发 `MEMORY_CLEAR`；返回命令状态 |
| `POST /v1/parent/devices/{id}/memories/resume` | 家长会话 + CSRF | 设备拒绝/过期后"恢复同步" |
| `GET /v1/voice/{id}/summary` | 语音 token，仅内部地址 | §7.3 最小摘要 |

家长接口响应都带 `Cache-Control: no-store`。语音 token 读 `Settings.voice_token`（S5 引入 `KID_PET_VOICE_TOKEN`）；未配置时摘要接口一律 401。

## 快照规则

- 请求体 ≤ 5 KiB（超出 413）。方案 §9 写的是 4 KiB，但所有字段取上限时的紧凑 JSON 是 4440 字节（测试 `test_worst_case_snapshot_fits_the_limit`），所以放宽到 5120 并要求紧凑 JSON。`schema_version` 只认 `"2.0"`，`ledger_schema` 只认 `3`；整数严格（布尔、小数、数字字符串都拒绝），未知字段 422。
- 重复上传先只读判断：完全相同的重发（且没有新命令要送达）、10 s 内重复的 `stale`、限流内的新 generation 都不开写事务。同一设备同一种解码失败每小时只记一条审计。
- `memory_b64`（288 字节）和 `parent_settings_b64`（28 字节）严格解码；任何失败整份快照 422（`MEMORY_FORMAT_UNSUPPORTED` / `MEMORY_CORRUPT` / `SETTINGS_VERSION_UNSUPPORTED` / `SETTINGS_CORRUPT` / `BASE64_INVALID`），不更新任何状态，审计 `state.reject`。未知格式版本拒绝，不猜。
- 幂等键 `(device_id, generation)`。`state_sha` 只覆盖**账本提交的内容**（`ledger_schema`、`pet`、`memory_b64`、`parent_settings_b64`、`parent_settings_rev`）：更大 → `accepted`；相等且账本内容相同 → `duplicate`，同时刷新不经账本的字段（电量、时钟、清单、TF 空间、`awaiting_confirm`）并处理回执；相等但账本内容不同 → `generation_conflict`（不更新、审计、面板告警 7 天）；更小 → `stale`（连续 3 次 → 面板提示"重新开通"）。四种都是 HTTP 200，看 `result`。
- 新快照与上一份已接受快照间隔 < 10 s → 429 + `Retry-After: 10`（重复重发不受限）。
- 每日归档：上一份的"今天"在设备换日后写入 `daily_summaries`，随后用新快照里设备自己的"昨天"覆盖同一天（更完整）。离线超过 2 天中间几天只剩成长总数（方案已接受）。
- 记忆投影只存数字 id 和计数；喜好的 `source_event` 不落库。

## 清空记忆（§5.4 墓碑 + 纪元）

家长删除 → 服务端立即删 `memory_current`、`daily_summaries`，`clear_epoch += 1`，下发 `MEMORY_CLEAR`（需设备上家长确认，7 天过期）。之后：

| 状态 | 快照里的记忆 |
|---|---|
| `pending`（排队/已送达/设备上待确认） | 丢弃（宠物、电量照常更新） |
| 设备回执 `applied` | 带回执的这一份仍丢弃；**之后更高 generation** 的快照恢复接受 |
| 回执 `rejected` / 服务端超时 `expired` | 继续丢弃，面板提示"设备上的记忆没有清空"；家长可再删一次（新纪元、新命令）或"恢复同步" |

清空命令在 `pending` 时重复删除是幂等的（同一条命令）。服务端已判超时的命令，设备之后仍可回执 `applied`/`rejected`（设备时钟未知时可能晚执行清空，设备说了算）；收到 `applied` 时服务端再删一次记忆副本，防止"恢复同步"期间写回的旧记忆留下。设备给出的最终结果不会被覆盖。性格（temperament）不属于记忆，墓碑期间照常更新。

## 保留期（D6）

`StateSync.run_retention()`：先把过期的 `pending` 命令标为 `expired`，再删完成超过 30 天的命令、365 天前的审计。随设备上报每天最多跑一次（`maintenance_runs` 记时间），S7 可另加定时器。**永不触碰** `memory_current`、`daily_summaries`、`memory_clears`、`device_state`——这些只在家长清空记忆或删除设备时删除（所有 S2 表对 `devices` 都是 `ON DELETE CASCADE`）。

## 固件需要发送 / 接受的内容（§11.2 设备端配套）

**发送** `PUT /v1/devices/{id}/state`，`Authorization: Bearer <token>`，**紧凑 JSON**（cJSON `cJSON_PrintUnformatted`）≤ 5120 字节，最坏情况 4440 字节：

| 字段 | 要求 |
|---|---|
| `schema_version` | 固定 `"2.0"` |
| `generation` | 账本提交计数，1..2^63-1，只增不减；没有新的账本提交时重发同一个值（只改电量/待确认列表等不需要加一） |
| `ledger_schema` | `3` |
| `fw_version` | 1–40 个 `[A-Za-z0-9._+-]` |
| `rtc_day` | 本地日期距 1970-01-01 的天数（与 `MemoryEvent::day` 同一约定），时钟未知为 0 |
| `pet` | `PetSnapshot`（`pet_controller.h`）：`mode` ∈ `IDLE INTERACT STORY LISTENING SPEAKING QUIET`，`mood` ∈ `NEUTRAL HAPPY CURIOUS THINKING SLEEPY`（枚举名大写），`interaction_count` u32，`active_page` 1..65535，`quiet` bool |
| `memory_b64` | `MemoryStore::Encode` 的 288 字节，标准 base64（384 字符，带填充） |
| `parent_settings_b64` | `ParentSettingsCodec::Encode` 的 28 字节（40 字符） |
| `parent_settings_rev` | u32（v2 codec 之前可固定 0） |
| `battery` | `{"pct": 0..100, "charging": bool}` |
| `inventory` | ≤ 32 项 `{"kind":"book"|"soundpack"|"game","content_id":u16,"revision":u16,"header_crc":"8 位小写 hex"}` |
| `sd_free_kib` | 可选，u32 |
| `awaiting_confirm` | ≤ 8 个正在家长入口等待确认的 `command_id`（1..2^53-1） |
| `applied_commands` | ≤ 16 个 `{"command_id","result":"applied"|"rejected"|"expired"}` |

不允许其他字段；整数必须是 JSON 整数（不能是 `true`、`5.0` 或 `"5"`）。

**何时上传**（都去抖 30 s，两次新 generation 至少间隔 10 s）：联网后一次；账本提交后；**回执列表或待确认列表变化后**（没有账本提交就用同一个 generation 重发，服务端按 `duplicate` 刷新）；关机前一次（如果网络在）。离线不排队，下次只发最新。

**接受** 响应（HTTP 200）：

```json
{"result": "accepted|duplicate|stale|generation_conflict", "generation": 1842, "server_rev": 31,
 "acked_commands": [4097],
 "commands": [{"command_id": 4098, "kind": "MEMORY_CLEAR", "expires_at": 1794204800,
               "requires_confirm": true, "payload": {"clear_epoch": 3}}]}
```

- `applied_commands` 里的回执要一直带着，直到某次响应的 `acked_commands` 包含它（`accepted`/`duplicate` 才会确认；`stale`/`generation_conflict` 不处理回执）。
- `command_id` 是 1..2^53-1 的随机数：最高位为 0（`IsExternalEventId`），且 cJSON 的 double 能精确表示。用它作为账本事件 id → 重放窗口去重。
- **是否需要家长确认由设备按 `kind` 决定，忽略 `requires_confirm`**（它只供显示）：`MEMORY_CLEAR`、`SET_TEMPERAMENT` 永远要在设备家长入口确认（D5），即使收到 `requires_confirm: false` 也一样；服务端也从不对这两种发 `false`。待确认的命令只进内存、放进 `awaiting_confirm`，家长确认后才以 `MemorySource::Parent` 提交，回执 `applied`。
- 家长**拒绝**要持久化（写账本或 NVS，不能只在内存）：重启后同一命令再次送达时直接回 `rejected`，不再问家长。
- 再次送达的命令若已在重放窗口里（账本判为 Duplicate），回执 `applied`（它当初就是被执行了）。
- `expires_at` 是 Unix 秒；设备时钟已知且已过期 → 不执行，回 `expired`。时钟未知时只执行幂等命令（清空、性格、内容同步）。
- 未知 `kind` 忽略并回 `rejected`。错误：401 token 不对，413 太大，422 格式/解码错误（`detail` 是代码，不重试同一内容），429 按 `Retry-After` 等待。
- `GET /v1/devices/{id}/commands` 返回 `{"server_rev","commands":[...]}`，格式同上。

## 测试

```sh
py -3.11 -m unittest tests.test_v5_server_memory_codec tests.test_v5_server_state_sync -v
bash scripts/p2_test.sh   # 含 tests/cpp/test_memory_golden.cc，校验 tests/fixtures/memory/ 与 C++ 逐字节一致
```

黄金向量改动流程：改 C++ codec 后运行 `build/p2/test_memory_golden --write tests/fixtures/memory`，再同步改 `memory_codec.py`，两边测试都过才算完成。

## 本阶段未做 / 交给后续

- 清空记忆的"再次输入密码 + TOTP"（§9 敏感操作）：需要 `ParentAuth` 提供统一的二次验证原语（删除设备、备注也要用），本 PR 只要求会话 + CSRF；设备上的家长确认（D5）仍是必经的第二道。
- `SET_TEMPERAMENT`、`SET_PARENT_SETTINGS`、`SELECT_VOICE_PACK` 已在命令通道里（含确认/过期规则），家长接口和保守合并由 S3 提供；`SYNC_CONTENT` 由 S4 下发。
- 喜好名取自 S4 的 `like_topics` / `like_options`，书名取自 S4 的 `content_packages.title`（kind=book，取最新修订），都只认 `approved` 的包；`name` 取自 S3 的 `profiles`（表存在时）。没有审核名的 id 一律省略。

## 命令下发窗口（S3 补充）

每次响应（快照响应和 `GET .../commands`）最多带 **8 条**命令，按创建顺序；设备回复缓冲上限是 16 条 / 768 个 JSON 节点 / 12 KB，服务端刻意取更保守的 8 条。快照里 `awaiting_confirm` 列出的命令（设备上正等家长确认）**不再重复下发**，免得它们占满窗口、让后面的命令一直发不出去；它们在服务端仍是 `pending`，面板显示"等待在设备上确认"，设备忘记它们（不再列在 `awaiting_confirm`）后会重新下发。已发出但设备还没回执的命令会在后续响应里继续出现，直到回执或过期。只有真正下发出去的命令才记 `delivered_at`。
