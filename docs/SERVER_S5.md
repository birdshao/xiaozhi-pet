# 服务端 S5：语音意图与页问答

实现 `V5_V6_SERVER_PLAN.md` §6.1–§6.3、§11.1 S5。不调用任何 LLM；开放问答（S6）未实现，开放问答开关对本接口**无效**。

## 接口

| 路由 | 鉴权 | 说明 |
|---|---|---|
| `POST /v1/voice/{id}/turn` | 语音服务 token（`KID_PET_VOICE_TOKEN`，32–256 字符，与任何设备 token 分开） | 请求 `{"text","turn_id"?}`，响应 `{kind, tts_text, action, reason}` |
| `PUT /v1/devices/{id}/context` | 设备 token | `{scene: HOME|READER|GAME, page, content_id?, revision?, header_crc?, book_id?}`，只存内存 120 s |

语音 token 只对 `/v1/voice/*` 有效（设备、家长路由全部 401），设备 token 对语音路由 401。未配置 `KID_PET_VOICE_TOKEN` 时语音路由拒绝所有请求。应用层再设一道**白名单**：直连 peer 必须在 `KID_PET_VOICE_ALLOWED_PEERS`（逗号分隔 IP/CIDR，默认 `127.0.0.1,::1`；compose 里填小智容器所在网段）内，且不得带 `X-Forwarded-For`/`Forwarded`/`X-Real-IP`、不得等于 `KID_PET_TRUSTED_PROXY`，否则 403（语音路由不得经公网入口，§3.3 N6）。响应带 `X-Voice-Signature` = HMAC-SHA256(语音 token, `turn_id + "\n" + 响应体`)，守卫验证后才朗读。`{id}` 先按 `DEVICE_RE` 校验，不合法的 id 不查库也不写审计。"只读摘要"路由属于 S2，尚不存在。

## 处理顺序（`voice.py`、`intent.py`）

1. 文本检查（固定命令里的动作类还要求"像命令"：含 `?`/`？` 或"喂"后紧跟标点/空白——如"团团睡觉？"、"喂，团团！"——都不匹配；几点了/电量带问号照常匹配）：JSON、只含 `text`/`turn_id`、`turn_id` 8–64 位 `[A-Za-z0-9_-]`、去空白后 1–120 字、无控制/格式字符 → 否则 `fallback/text_invalid`（HTTP 200，固定句）。
2. 未知设备 → `fallback/unknown_device`。
3. 安静时段（设备投影 `quiet`）→ `fixed/quiet_hours`。
4. 固定命令（NFKC + 去空白/标点 + 精确匹配白名单）：`SLEEP_REQUEST PAGE_NEXT PAGE_PREV OPEN_READER OPEN_GAME PET_FEED PET_GROOM GO_HOME`（`local_command`，`action` 仅来自这里）；几点了；还有多少电。
5. 页问答：白名单问句 + 120 s 内的 READER 上下文 → 只播已审核页文本（`approved_content/page_qa`）。
6. 其余 → 固定兜底句 `fallback/no_match`。

任何异常（含审计写失败以外的所有内部错误）→ `fallback/internal_error` 固定句。`local_command`、页问答、兜底都不带 ASR 回显。

## 审计与隐私

每个 turn 写一行 `audit_log(actor=voice, action=voice.turn, target=<device_id>, result=<kind>:<reason>)`，日志只有 `kind/reason/耗时`。ASR 文本、`turn_id`、回复文本都不落盘、不进日志（测试断言）。审计写失败不影响应答。

## 已解决的方案歧义

| 歧义 | 处理 |
|---|---|
| `routers/voice.py`、`intent.py` 已存在（S0 的旧 `/intent`） | 旧接口、`resolve()`、既有测试不动；新路由加在 `routers/voice.py`，匹配器加在 `intent.py`，turn 流水线在新的 `voice.py`；`api.py` 只加 import + 一行 `app.state.voice_turns` |
| 安静时段来源（家长作息属 S3） | 用现有设备投影 `devices.quiet`；S3 之后改读家长作息。严格按 §6.1 顺序：安静时段在固定命令之前，所以安静时说"团团睡觉"也只回安静句（设备此时本就在休息） |
| 时区（家长配置属 S3） | 配置项 `KID_PET_TIMEZONE`（默认 `Asia/Shanghai`），S3 改为按设备；新增依赖 `tzdata`（slim 镜像/Windows 没有系统时区库）。整点说"现在是{h}点整。"，否则"现在是{h}点{m}分。"，小时用 24 小时制，槽位只有整数 |
| 电量（S2 未合并） | `DeviceStateBattery` 读 §4.2 的 `device_state(device_id, battery_pct, received_at)`；表不存在/类型不对/百分比越界/快照超过 600 s（或来自未来）→ "看看屏幕右上角的电池吧。"。S2 若改了列名需同步这里 |
| 页内容来源（S4 未合并）、设备只知道 `content_id/revision` | `context` 接受 `content_id/revision/header_crc` 并缓存，但目前只有 `book_id`（旧 `pages` 表，已审核）能解析；`PageSource.page_text()` 是 S4 替换点（按 `(content_id, revision, header_crc)` 查 `content_pages`）。解析不到 → "我们先翻到书里再问吧。" |
| 开放问答关闭时页问答范围 | 与现有行为一致：只有白名单问句（这一页讲了什么/这页讲了什么/当前页是什么/读这一页/这一页是什么）读回该页审核原文；其他问句走固定兜底 |
| 每条命令的回话 | §6.2 只给了睡觉/喂食的句子；其余按同样口吻写成固定句（见 `intent.py`）。守卫只在设备接受（`done`/`deferred`）时说这句，否则按设备结果说固定句（见下）。喂食句后来改为"好呀，选一样好吃的喂团团吧。"：语音只打开选菜画面（[V6_VOICE_COMMANDS](V6_VOICE_COMMANDS.md)） |
| 同义词 | 只加了少量明显同义（喂食、讲个故事、玩个游戏…），全部列在 `intent.py`；近似句（"我不想睡觉"、"回家吃饭"）精确匹配不上 |
| 语音限流 1 次/s（§9）、页问答的包头 CRC 校验（需 S4）、审计保留清理（S7） | 未做，记为后续 |
| 上游钉住的提交（P3 文档/overlay 是 f196516…，`version-lock.json` 是 v0.9.8 `8f2dda09…`） | overlay 与 P3 文档改钉 `8f2dda09…`，已在该提交上对 `intentHandler.py` dry-run 通过（锚点唯一，`speak_txt` 在同文件）；测试断言 overlay 的钉与 `version-lock.json` 一致 |

## 守卫（`child_voice_guard.py`）

只做：儿童模式开关、板子绑定检查、把 ASR 文本 POST 到 `/v1/voice/{business_id}/turn`、朗读返回的 `tts_text`（`kind` 只认 `fixed/approved_content/fallback`，长度 ≤320），以及把 `local_command` 交给设备。不再带设备 token，改用 `KID_PET_VOICE_TOKEN`。

`local_command`（已在固件 v2.5.2 / 服务端 v0.9.8 上核对调用路径）：守卫在单独的 asyncio 任务里用上游的 `call_mcp_tool(conn, conn.mcp_client, "self_pet_request", {"action": <名字>}, timeout=3)` 调设备端 MCP 工具 `self.pet.request`，按设备回的 `result` 选话术：`done`/`deferred` 说服务端的固定句（`deferred` 另设 `conn.close_after_chat`，说完断开会话，设备空闲后再开画面），拒绝类结果说对应的固定句，没有 MCP / 没有工具 / 超时 / 报错 / 回包不对时退回原来的"我听到了，我们用设备上的按键完成这个动作吧。"。`action` 必须是 `[A-Z_]{1,15}`，否则按服务异常处理、不调设备；白名单在设备上（= `intent.ACTIONS`）。不放在 ASR 协程里等，是因为手动拾音模式下 ASR 在 websocket 接收循环里被 await，设备回包也走这个循环。完整的结果→话术表见 [V6_VOICE_COMMANDS](V6_VOICE_COMMANDS.md)。overlay 补丁本身没变（仍是一处 5 行插入）。

`KID_PET_API_URL` 的主机在代码里固定为：回环（`127.0.0.1`/`localhost`/`::1`），或**字面**私有/链路本地 IP（compose 里给 pet-learning 固定一个 IP，例如 `http://172.20.0.10:8780`）；不接受主机名（路由器 DNS 可能把单标签名解析到局域网设备），没有放宽用的环境变量。仍要求 `http`、无账号/路径/查询，畸形 URL/端口 → 拒绝。守卫用语音 token 校验响应签名（绑定本次 `turn_id`），签名缺失/不符 → 固定句 `NO_SERVICE`。overlay 仍只有一处锚点，合约测试断言补丁只是一段 5 行插入。

## 设备端仍需要（不在本 PR）

- BOOT 开始说话时 `PUT /v1/devices/{id}/context`（含 `content_id/revision/header_crc`；S4 后按它解析页）。
- ~~设备端 MCP 工具 `self.pet.request(action)`~~ 已完成：只接受 `intent.ACTIONS` 的 13 个名字，调用路径已在锁定版本上核对，守卫已改为真正调用（见上与 [V6_VOICE_COMMANDS](V6_VOICE_COMMANDS.md)）。
- ~~设备端对 `PAGE_PREV`、`OPEN_GAME`、`PET_FEED`、`PET_GROOM`、`GO_HOME` 的分发~~ 已完成（`PET_FEED` 打开选菜画面，画面类命令在对话结束后打开）。真机上的语音往返待 voice-harness 回归。
- S2 快照上报电量（`device_state`）。

## 部署（S7）需要

小智服务端环境：`KID_PET_VOICE_TOKEN`、`KID_PET_API_URL=http://<pet-learning 固定 IP>:<port>`、`KID_PET_BUSINESS_DEVICE_ID`；PetLearning 环境：同一个 `KID_PET_VOICE_TOKEN`、`KID_PET_VOICE_ALLOWED_PEERS=<小智容器网段或 IP>`、`KID_PET_TIMEZONE`。
