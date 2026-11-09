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
3. 安静时段（设备投影 `quiet`）→ `fixed/quiet_hours`（唤醒词问候也受此限制，安静时只回安静句）。
3b. 唤醒词问候（`intent.is_wake_only`）：板子唤醒后会把"你好小智"本身当作第一句话送来。NFKC + 去空白/标点后，文本**完全**等于 `你好小智`/`你好团团`（及误识 `你好小知`/`你好晓智`），或名字在前的 `团团你好`/`小智你好`，前后/中间可夹"团团、呀、啊、哦、哇、啦、嘿、哎"这类语气词（如"你好呀团团""团团你好小智啊"），→ `fixed/wake_greeting`，固定句"我在呢。"。整句匹配，不做包含匹配："你好小智今天天气怎么样""你好团团讲故事"仍走后面的流程。
4. 固定命令（NFKC + 去空白/标点 + 精确匹配白名单，外加一张**显式别名表**：只收真机基线里确认过、且不会成为别的句子的误识——`团睡觉`→`SLEEP_REQUEST`、`打开书下`/`书架`/`打开书架`/`去书架`/`看书架`→`OPEN_READER`（V9 起"讲故事"在卡上有两本及以上时先出书架，所以书架就是这个动作）；不做编辑距离或包含匹配，也不收可能是真问题的句子（如"还有多少遍"：刷牙、游戏时也会问，不当作电量），"一页""半业""一年了""松下"这类太短或有歧义的不收；问号规则不变，"我不想睡觉""不想睡觉""我要睡觉了吗？"都不触发）：`SLEEP_REQUEST PAGE_NEXT PAGE_PREV OPEN_READER OPEN_GAME PET_FEED PET_GROOM GO_HOME`（`local_command`，`action` 仅来自这里）；几点了；还有多少电。
5. 页问答：白名单问句 + 120 s 内的 READER 上下文 → 只播已审核页文本（`approved_content/page_qa`）。
6. 其余 → 固定兜底句 `fallback/no_match`。兜底句（`no_match`、`text_invalid`、`internal_error`）统一为一句短话"没听清，再说一遍好吗？"（约 2 s；原句两段、约 7 s）。安静/未绑定/无电量/无页上下文等其他固定句都在 17 字内，未改（测试断言长度）。守卫侧 `REVIEWED_FALLBACK`（服务不可用时的离线句）和 `kind`/长度限制不变。

任何异常（含审计写失败以外的所有内部错误）→ `fallback/internal_error` 固定句。`local_command`、页问答、兜底都不带 ASR 回显。

## 审计与隐私

每个 turn 写一行 `audit_log(actor=voice, action=voice.turn, target=<device_id>, result=<kind>:<reason>)`，日志只有 `kind/reason/耗时`：`voice_turn kind=… reason=… ms=…` 以 INFO 级别输出——`create_app` 把 `pet_learning` logger 设为 INFO，并在根 logger 没有 handler（uvicorn 只配自己的 logger）时给它挂一个 stderr handler，所以 `docker logs pet-learning` 里能直接按 kind 统计。ASR 文本、`turn_id`、回复文本都不落盘、不进日志（测试断言）。审计写失败不影响应答。

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

`local_command`（已在固件 v2.5.2 / 服务端 v0.9.8 上核对调用路径）：守卫在单独的 asyncio 任务里用上游的 `call_mcp_tool(conn, conn.mcp_client, "self_pet_request", {"action": <名字>}, timeout=3)` 调设备端 MCP 工具 `self.pet.request`，按设备回的 `result` 选话术：`done`/`deferred` 说服务端的固定句（`deferred` 另设 `conn.close_after_chat`，说完断开会话，设备空闲后再开画面），拒绝类结果说对应的固定句，没有 MCP / 没有工具 / 超时 / 报错 / 回包不对时退回原来的"我听到了，我们用设备上的按键完成这个动作吧。"。`action` 必须是 `[A-Z_]{1,15}`，否则按服务异常处理、不调设备；白名单在设备上（= `intent.ACTIONS`）。不放在 ASR 协程里等，是因为手动拾音模式下 ASR 在 websocket 接收循环里被 await，设备回包也走这个循环。完整的结果→话术表见 [V6_VOICE_COMMANDS](V6_VOICE_COMMANDS.md)。`intentHandler.py` 的补丁没变（仍是一处 5 行插入）；overlay 另在 `mcp_client.py`、`mcp_executor.py` 各插一处，不论模式都让大模型看不到、调不了 `self_pet_request`（ADR-008）。

`KID_PET_API_URL` 的主机在代码里固定为：回环（`127.0.0.1`/`localhost`/`::1`），或**字面**私有/链路本地 IP（compose 里给 pet-learning 固定一个 IP，例如 `http://172.20.0.10:8780`）；不接受主机名（路由器 DNS 可能把单标签名解析到局域网设备），没有放宽用的环境变量。仍要求 `http`、无账号/路径/查询，畸形 URL/端口 → 拒绝。守卫用语音 token 校验响应签名（绑定本次 `turn_id`），签名缺失/不符 → 固定句 `NO_SERVICE`。overlay 仍只有一处锚点，合约测试断言补丁只是一段 5 行插入。

### 有字没声、对话停不下来（2026-11-09 真机，已修）

现象（串口 `human-wake2.log`，固件 e50c1e8 / 服务端 870a87a）：唤醒后 `listening -> speaking -> listening` 在同一毫秒；之后板子一直停在 listening，却不断收到 `<< 没听清` / `<< 再说一遍好吗？`，没有声音，20 多分钟不结束。

根因（上游 xiaozhi-esp32-server `8f2dda09`，xiaozhi-esp32 v2.5.2 `49b4380b`）：

1. **守卫的回复从不发 `tts start`。** 服务端只在 `core/handle/sendAudioHandle.py:346`（`send_stt_message` 末尾）发 `{"type":"tts","state":"start"}`；正常聊天由 `receiveAudioHandle.py:102` 调它，但守卫在 `intentHandler.py:31` 的锚点处就返回了 True，`startToChat` 在 `:97-99` 直接 return。守卫用的 `speak_txt`（`intentHandler.py:233-252`）只让 TTS 线程发 `sentence_start`（`sendAudioHandle.py:43`）、音频和 `stop`（`:53`）。板子只在收到 `start` 时进 speaking（`application.cc:624-627`），只在 speaking 时把下行音频放进解码队列（`application.cc:554-557`），`stop` 也只在 speaking 时才处理（`:631`）。所以文字照显示，音频全丢，板子一直 listening。
2. **对话停不下来**是 1 的后果：板子一直 listening（自动拾音）就一直上传环境声，ASR 每出一段文字，PetLearning 回 `no_match` 兜底；无声超时（`receiveAudioHandle.py:110-124`）被 VAD 的"有声"不断重置，永远到不了。
3. **第一次唤醒没回应**：板子发 `listen detect "你好小智"`，`enable_greeting: false` 时上游走 `listenMessageHandler.py:101-105`：只发 stt（`>> 你好小智`）+ start + 立即 stop，所以 speaking→listening 同一毫秒，守卫根本没收到这句。2.6 s 后的"没听清"是下一段 ASR（该分支不设 `just_woken_up`，VAD 不静音）。后面几次"我在呢"是孩子在 listening 中再说"你好小智"，走了 ASR → 守卫 → `wake_greeting`。
4. 顺带：上游 `handleAbortMessage` 置 `client_abort = True`，只有聊天路径（`receiveAudioHandle.py:105`）会清掉；TTS 线程在它为真时丢掉一切（`tts/base.py:374,432`）。守卫路径以前从不清，所以一次打断后守卫的回复也会一直没声。

修法（只改守卫和 overlay）：

- `_speak` 改为一整轮：换 `sentence_id`、`await send_tts_message(conn, "start")`、置 `client_is_speaking`，再 `speak_txt`；结束的 `stop` 仍由上游在最后一句后发。命令回复、告别句都走它。
- **打断**：`client_abort` 在 `handle_child_guard` 入口清掉，时机与上游聊天路径一致（`startToChat` 先打断正在播的回复 `receiveAudioHandle.py:91-92`，再在 `:105` 清掉）。守卫等 PetLearning（最长 9 s）或等板子执行命令（3 s + 最多 4 s）期间，孩子用唤醒词打断（上游 `handleAbortMessage` 置位），`_speak` 看到 `client_abort` 就放弃这一句，不发 start、不朗读（auto 模式下 ASR 经 `run_coroutine_threadsafe` 调 `startToChat`，与 websocket 接收并发，见上游 `asr/base.py:43-51`）。若打断恰好落在发 start 的 await 期间，守卫自己再发一个 `stop` 并复位 `client_is_speaking`，因为上游此时会丢掉整句、不再发 stop。
  - 轮次：只看 `client_abort` 不够——A 轮还在等开放聊天时孩子打断并说了 B，B 进入守卫会清掉 abort，A 的回复后到仍会播出。所以每轮进入守卫时 `conn.child_guard_turn += 1` 并记下本轮编号，`_speak`（含命令任务）在发 start 前后都核对"编号仍是最新、且没被打断"，否则放弃；过期的回复也不计入兜底计数。
  - "说完就结束"（安静句、第 3 次告别、命令 `deferred`/`pending`）只在确定朗读、发出 start 之后才置 `close_after_chat`。上游 `handleAbortMessage` 会先清这个标志；若守卫在 await 之后再置位、这句又被丢弃，标志会残留，下一条正常回复说完就断开，180 s 无声超时也失效。
- **会话结束**：PetLearning 的 `kind=fallback` 且 `reason` 是 `no_match`/`text_invalid`，以及守卫本地的 `NO_SERVICE`/`NOT_BOUND`，算"没有有效意图"。连续 `UNCLEAR_LIMIT = 3` 次时，第 3 次不再说"没听清"，改说告别句"想找我就再叫我一声哦，拜拜。"（告诉孩子怎么把宠物叫回来）并置 `conn.close_after_chat`，上游在这句的 `stop` 之后断开（`sendAudioHandle.py:52-55`）。任何有效回复（固定句、命令、页问答、开放聊天的回答或拦截句等其他 `reason`）都把计数清零。环境噪声在真机上每 2–5 s 出一段，最多三段（约 6–15 s）就结束，再唤醒即可。不做总时长上限：读书可以合理地很长，证据里的长会话全是兜底句。
  - 取舍：`no_match` 不只代表噪声。**开放聊天关闭时**，孩子问了不支持的问题也是 `no_match`；阈值 3 意味着孩子连问两个不支持的问题仍会得到"没听清，再说一遍"，连续第三个才告别（可再唤醒）。**开放聊天打开时**，大多数真问题由 GLM 回答、计数清零，阈值主要拦噪声；但噪声若被 ASR 识别成像样的文字、GLM 也给了回答，同样会清零。**已知限制、待真机观察**：开放聊天打开时，噪声循环是否还会被 GLM 回答撑住，没有证实。
- **安静时间**：安静时段里 PetLearning 对任何文本都回 `fixed/quiet_hours`（`voice.py` 的 quiet 分支），这是有效回复、会清零计数，噪声会让板子每隔几秒说一次安静句。守卫在 `reason == "quiet_hours"` 时说完这一句就置 `close_after_chat` 结束会话。
- `close_connection_no_voice_time: 180`（`deploy/xiaozhi/child.config.yaml`，上游默认 120）：给孩子想一想、翻页的时间。它只管"安静"，噪声会重置它，所以结束噪声会话靠的是上一条。
- 唤醒词（S6 审计 #12）：overlay 在 `listenMessageHandler.py` 的 `if is_wakeup_words and not enable_greeting:` 前插一段，只在儿童模式下把板子发来的唤醒词原文交给 `startToChat` → 守卫，PetLearning 的 `is_wake_only` 回"我在呢。"，并设 `just_woken_up`。匹配没有放宽：上游问候分支用的替身文本"嘿，你好呀"仍不是唤醒词；`wakeup_words` 列表和 `enable_greeting: false` 不变。

已知限制：

- 儿童模式的唤醒分支不再发 `stt`，所以固件 `display_->UserUtterances()`（脸部 thinking 计数）在儿童模式下不会因唤醒词增加；之后的 ASR 文本走守卫，也不发 stt。暂不改，记为已知问题。
- 唤醒后约 2 s 内的上行音频被 `just_woken_up` 丢弃（上游 `receiveAudioHandle.py:21-26`，与上游问候分支相同）。孩子连着说"你好小智，几点了"时，后半句可能丢失，要停一下等"我在呢"再说。
- 开放聊天打开时噪声是否会被 GLM 回答撑住（见上），待真机观察。

真机仍需验证：唤醒后串口应是 `connecting -> listening -> speaking`（"我在呢"有声）`-> listening`；每条回复前都有 `listening -> speaking`；在嘈杂环境不说话，三句后听到"想找我就再叫我一声哦，拜拜"并回 idle，服务端日志 `child guard: 3 unclear turns in a row`；说话中用唤醒词打断，被打断那句不再播出；安静时段说一句话，只听到一次安静句后回 idle。

## 设备端仍需要（不在本 PR）

- BOOT 开始说话时 `PUT /v1/devices/{id}/context`（含 `content_id/revision/header_crc`；S4 后按它解析页）。
- ~~设备端 MCP 工具 `self.pet.request(action)`~~ 已完成：只接受 `intent.ACTIONS` 的 13 个名字，调用路径已在锁定版本上核对，守卫已改为真正调用（见上与 [V6_VOICE_COMMANDS](V6_VOICE_COMMANDS.md)）。
- ~~设备端对 `PAGE_PREV`、`OPEN_GAME`、`PET_FEED`、`PET_GROOM`、`GO_HOME` 的分发~~ 已完成（`PET_FEED` 打开选菜画面，画面类命令在对话结束后打开）。真机上的语音往返待 voice-harness 回归。
- S2 快照上报电量（`device_state`）。

## 部署（S7）需要

小智服务端环境：`KID_PET_VOICE_TOKEN`、`KID_PET_API_URL=http://<pet-learning 固定 IP>:<port>`、`KID_PET_BUSINESS_DEVICE_ID`；PetLearning 环境：同一个 `KID_PET_VOICE_TOKEN`、`KID_PET_VOICE_ALLOWED_PEERS=<小智容器网段或 IP>`、`KID_PET_TIMEZONE`。
