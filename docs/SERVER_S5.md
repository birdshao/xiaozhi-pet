# 服务端 S5：语音意图与页问答

实现 `V5_V6_SERVER_PLAN.md` §6.1–§6.3、§11.1 S5。不调用任何 LLM；开放问答（S6）未实现，开放问答开关对本接口**无效**。

## 接口

| 路由 | 鉴权 | 说明 |
|---|---|---|
| `POST /v1/voice/{id}/turn` | 语音服务 token（`KID_PET_VOICE_TOKEN`，32–256 字符，与任何设备 token 分开） | 请求 `{"text","turn_id"?}`，响应 `{kind, tts_text, action, reason}` |
| `PUT /v1/devices/{id}/context` | 设备 token | `{scene: HOME|READER|GAME, page, content_id?, revision?, header_crc?, book_id?}`，只存内存 120 s |

语音 token 只对 `/v1/voice/*` 有效（设备、家长路由全部 401），设备 token 对语音路由 401。未配置 `KID_PET_VOICE_TOKEN` 时语音路由拒绝所有请求。应用层再设一道防线：带 `X-Forwarded-For`/`Forwarded` 头，或直连地址等于 `KID_PET_TRUSTED_PROXY` 的请求返回 403（语音路由不得经公网入口，§3.3 N6）。"只读摘要"路由属于 S2，尚不存在。

## 处理顺序（`voice.py`、`intent.py`）

1. 文本检查：JSON、只含 `text`/`turn_id`、`turn_id` 8–64 位 `[A-Za-z0-9_-]`、去空白后 1–120 字、无控制/格式字符 → 否则 `fallback/text_invalid`（HTTP 200，固定句）。
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
| 每条命令的回话 | §6.2 只给了睡觉/喂食的句子；其余按同样口吻写成固定句（见 `intent.py`），守卫目前对所有 `local_command` 统一说"用按键" |
| 同义词 | 只加了少量明显同义（喂食、讲个故事、玩个游戏…），全部列在 `intent.py`；近似句（"我不想睡觉"、"回家吃饭"）精确匹配不上 |
| 语音限流 1 次/s（§9） | 未做，留给限流统一处理 |

## 守卫（`child_voice_guard.py`）

只做：儿童模式开关、板子绑定检查、把 ASR 文本 POST 到 `/v1/voice/{business_id}/turn`、朗读返回的 `tts_text`（`kind` 只认 `fixed/approved_content/fallback`，长度 ≤320）。不再带设备 token，改用 `KID_PET_VOICE_TOKEN`。`local_command` 在设备端 MCP 工具 `self.pet.request` 于锁定的小智版本上核对前，一律说"我听到了，我们用设备上的按键完成这个动作吧。"。

地址白名单改为配置项 `KID_PET_API_ALLOWED_HOSTS`（逗号分隔的精确主机名/IP，默认 `127.0.0.1,localhost,::1,pet-learning`，已含 compose 服务名）。仍要求 `http`、无账号/路径/查询。overlay 仍只有一处锚点，合约测试断言补丁只是一段 5 行插入。

## 设备端仍需要（不在本 PR）

- BOOT 开始说话时 `PUT /v1/devices/{id}/context`（含 `content_id/revision/header_crc`；S4 后按它解析页）。
- 设备端 MCP 工具 `self.pet.request(action)`，只接受上面 8 个枚举，并在锁定的小智版本上核对调用路径；之后守卫才能把 `local_command` 转为真正的调用。
- 设备端对 `PAGE_PREV`、`OPEN_GAME`、`PET_FEED`、`PET_GROOM`、`GO_HOME` 的分发（目前被忽略，见 V6_VOICE_COMMANDS.md）。
- S2 快照上报电量（`device_state`）。

## 部署（S7）需要

小智服务端环境：`KID_PET_VOICE_TOKEN`、`KID_PET_API_URL=http://pet-learning:<port>`、`KID_PET_BUSINESS_DEVICE_ID`；PetLearning 环境：同一个 `KID_PET_VOICE_TOKEN`、`KID_PET_TIMEZONE`。
