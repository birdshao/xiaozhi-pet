# 服务端 S6：开放问答（V6-04/05）

状态：代码完成，待真机验证（2026-11-08）。决策依据：[ADR-015、ADR-016](DECISIONS.md)（用户 2026-11-08 决定：用智谱 GLM、不做内容审核、大模型可以请求板子动作）。原方案见 [V5_V6_SERVER_PLAN.md](V5_V6_SERVER_PLAN.md) §6.4、§6.5、§11，本文写的是改过之后的实际做法。

## 1. 一轮语音怎么走

```text
ASR 文本 → 长度/字符检查 → 安静时段? → 固定命令 → 页问答 → 开放问答（全部开关都开才走）→ 固定兜底
                                                              │
        本地硬规则查输入 ─不过→ 固定句（不调模型、不计入熔断）
               │
        GLM（角色设定 + 最小摘要 + 当前页 + 工具 self_pet_request[家长勾选的子集]）
               ├─ 出错（超时 / HTTP 错误 / 格式不对）→ 固定句，计 1 次 error
               ├─ 平台自带过滤（finish_reason=sensitive）→ 固定句，计 1 次 block
               ├─ 调了工具 → local_command（动作的固定确认句，不用模型的话）→ 守卫调设备 → 按设备结果选固定句
               └─ 文字回答 → 本地硬规则查输出 ─不过→ 固定句，计 1 次 block
                                          └─ 通过 → kind=open_chat，守卫验签后播报
```

- 入口还是 `POST /v1/voice/{id}/turn`（S5），代码在 `services/pet_learning/open_chat.py`。这个接口现在放到线程池里跑，模型最多等 6 s 也不会卡住事件循环；守卫等服务的超时从 3 s 改为 9 s。
- 每轮只发一条：系统提示 + 孩子这一句。不带历史，不带设备 id，不带家长备注。
- 原文和回复都不进日志、不进审计。审计只记 `kind:reason`；熔断表 `open_chat_outcomes` 只记时间和 `ok|error|block`。

### 三道开关（缺一不可）

| 开关 | 在哪 | 关着时 |
|---|---|---|
| 代码开关 `policy.OPEN_CHAT_SELECTABLE` | 代码 | 面板打不开，已经打开的也不调模型（`open_chat_unavailable=tts_audit`） |
| GLM 密钥 `KID_PET_GLM_API_KEY` | `deploy/providers.env` | 面板开关置灰（`provider_missing`），已经打开的也不调模型 |
| `policies.open_chat` | 家长面板，打开时要重新输入密码 + TOTP | 走固定兜底 |

代码开关在本 PR 里随 §3 的审计一起打开（`True`）。如果以后升级小智、需要重新审计，先把它改回 `False`。

## 2. 组成

| 文件 | 作用 |
|---|---|
| `chat_provider.py` | 可插拔提供方。`GlmProvider` 走 HTTPS `https://open.bigmodel.cn/api/paas/v4/chat/completions`（地址写死，不能配置，防止把孩子的话发到别处）；`FakeChatProvider` 给测试和演示用，不联网。 |
| `persona.py` | V6-04 角色设定。名字必须是 1–6 个汉字或 1–12 个字母，否则用“团团”；性格只从三段审核过的模板里选；最小摘要只放审核过的选项名。规则包括：最多三句、不超过八十字、不问个人信息、不讲吓人的事、不让保密、不知道就说问爸爸妈妈、不能声称自己做了动作、动作只能通过工具请求。 |
| `chat_rules.py` | 本地硬规则，输入和输出各查一遍。共同规则：电话（7 位以上数字；中文数字要求连续书写；递增递减的数数、整数如 1000000 不算）、地址（几号楼/单元/室、某路几号、“我/你（家）住在……小区/路”、门牌、住址）、链接/邮箱、敏感词表。只查输出的：用短语判断保密/排他（“我们的秘密”“别告诉爸爸”）、打听个人信息（“你家住在哪”“你叫什么名字”）、声称已经做了动作（“已经帮你打开”）。“小秘密”“给爸爸打电话”“小兔子住在小区”“我们马上开始吧”都能通过，有测试。去掉 Markdown、表情和零宽连接符；超过 320 字符，或模型因 token 上限被截断时，在最后一个句号处截断，没有句号就算格式错误。 |
| `open_chat.py` | 编排、工具白名单、熔断。 |
| `migrations/007_open_chat.sql` | `policies.llm_actions_json`、`policies.open_chat_disabled_at`、`open_chat_outcomes` 表。 |
| `panel/`（新标签“开放问答”） | 开关和风险说明（打开要勾“已阅读”并输入密码 + TOTP）、动作勾选、熔断状态和原因；没有密钥时置灰。 |

### 熔断

- 10 分钟内连续 3 次模型出错（超时，包括整次请求超过 6 s；HTTP 错误；输出格式不对：JSON 坏、控制字符、空回答、过长且没有句号；调了不在白名单里的动作；其他意外异常）→ 自动关闭，原因记为 `model_errors`。中间有一次成功就重新计数。
- 一天（家长配置的时区，自然日）内**输出**被拦 5 次（本地硬规则或平台过滤）→ 自动关闭，原因记为 `output_blocked`。原方案是“输出审核 block ≥ 5 次”，现在没有审核，改成硬规则拦截。**孩子的输入被硬规则拦下不计数**，因为那是孩子说的话，不是模型出了问题。
- 关闭时写一条审计：`server / open_chat.breaker / <设备> / <原因>`。之后只有家长在面板上重新打开才行，重新打开时计数清零。

## 3. TTS 出口审计（小智 v0.9.8，`8f2dda09`）

按 §6.5 的要求，把锁定版本里所有可能让设备发声的来源逐条列出来。在儿童模式下，每一条要么被守卫接管，要么被 overlay 或配置关掉。合约测试：`tests/test_p3_xiaozhi_child_voice_guard.py` 里的 `test_tts_egress_*` 和 `test_child_config_*`。另外设了环境变量 `XZ_UPSTREAM_SRC=<锁定版本 checkout>/main/xiaozhi-server` 时，会直接对真实上游文件跑一遍锚点检查（本 PR 已在本地对 `8f2dda09` 跑过）。

| # | 来源（file:line） | 触发 | 儿童模式下的处理 |
|---|---|---|---|
| #1 | `core/connection.py:1213` 正常 `chat()` 的 LLM 流式输出 | 守卫没接管时 | 守卫接管所有 ASR；overlay 让 `chat()` 在儿童模式下直接返回 |
| #2 | `connection.py:1190/1288` `direct_answer` 工具 | 同 #1 | 同 #1 |
| #3 | `connection.py:1223` LLM 出错时播报 `system_error_response` | 同 #1 | 同 #1；另外配置改成审核过的固定句，checklist 检查 |
| #4 | `connection.py:1145` LLM 初始化出错 | — | 不出声 |
| #5 | `connection.py:1413` 工具结果播报（再见、换角色、放音乐出错、工具超时） | 有 `func_handler` 时 | `nointent` 下没有 `func_handler`（checklist 要求 `nointent`），加上 #1 的 overlay |
| #6 | `connection.py:1503` REQLLM 工具再调 `chat` | 同 #5 | 同 #1 |
| #7 | `core/handle/intentHandler.py` `intent_llm` 结果 → `speak_txt` | 守卫之后 | 守卫在它前面并消费所有文本；`nointent` |
| #8 | `plugins_func/functions/play_music.py` 播放提示 + mp3 | 工具调用 | 没有 `func_handler` |
| #9 | `core/handle/helloHandle.py:116` 唤醒词缓存回复 | 守卫之后 | 守卫在前；配置 `enable_wakeup_words_response_cache: false` |
| #10 | `helloHandle.py:144` 后台刷新唤醒词回复 | 只由 #9 启动 | 同 #9 |
| #11 | `helloHandle.py:61` hello | — | 不出声 |
| #12 | `listenMessageHandler.py:101` 唤醒词 + 关闭问候 | — | 不出声 |
| #13 | `listenMessageHandler.py:110` 唤醒词 + 打开问候（“嘿，你好呀”进聊天） | `enable_greeting: true` | 配置为 false，checklist 检查 |
| #14 | `listenMessageHandler.py:116` 客户端 `detect` 文本 | — | 进守卫 |
| #15 | `listenMessageHandler.py:86` `[device_call]` 前缀文本**原样播报**，绕过守卫 | 客户端发送 | **overlay：儿童模式下直接丢弃** |
| #16 | `receiveAudioHandle.py:134` 无声超时把 `end_prompt` 当成孩子的话送进聊天 | 120 s 没声音 | **overlay：儿童模式下静默结束**；配置 `end_prompt.enable: false`（checklist 检查）；守卫也能认出这句提示词，不转发，只说固定告别句 |
| #17 | `intentHandler.py:53` 退出命令 | 守卫之后 | 不出声 |
| #18 | `receiveAudioHandle.py:140` 每日输出上限提示（在守卫**之前**） | 只有 manager-api 配置能开 | **overlay：儿童模式下不播**；`read_config_from_api: false` |
| #19 | `receiveAudioHandle.py:148` 绑定码录音 | 启动超过 1 s 还没设置绑定事件 | **overlay：儿童模式下不播** |
| #20 | `sendAudioHandle.py:297` 每次 TTS 结束的提示音 | `enable_stop_tts_notify: true` | 配置为 false，checklist 检查 |
| #21 | TTS 失败 | — | 不出声 |
| #22 | ASR 失败 | — | 不出声（空文本不进聊天） |

**结论**：儿童模式下设备能说的话只剩守卫的 `speak_txt` 一条路。守卫只说三种内容：签过名的 PetLearning 回复（`fixed`、`approved_content`、`fallback`、`open_chat`）、动作执行后按设备结果选的固定句，以及守卫自己的固定句。

## 4. 大模型调动作（ADR-016）

- 工具名就是小智对 `self.pet.request` 消毒后的 `self_pet_request`，参数 `action` 的枚举 = 家长勾选的子集。子集为空时不给工具。
- 默认勾选：讲故事、上一页、下一页、玩游戏、回首页、喂团团、梳毛、唱歌、刷牙、洗手；不勾睡觉（另外“看看团团饿不饿”和“讲笑话”也默认不勾）。存在 `policies.llm_actions_json` 里，`NULL` 表示用默认值。
- 模型调了工具：服务端不用模型的话，回 `kind=local_command`，带这个动作在固定命令表里的确认句，`reason=open_chat_tool`。守卫走 #114 原样的流程：调设备 → 设备接受时说确认句，拒绝时说设备结果对应的固定句（安静时段、低电量、睡觉、家长菜单……）。所以说出来的话不可能和设备结果矛盾。
- 不在子集里的动作：不执行，回固定句，计 1 次 error。
- 板子的规则不变：语音不写记忆、不发奖励；“喂团团”只打开选菜画面。
- **#114 的过滤保留**：小智上游的 LLM 函数表在任何模式下都看不到 `self_pet_request`，模型点名调用也会被拒绝（`mcp_client.py`、`mcp_executor.py` 两处 overlay 不变）。这个工具只在 PetLearning 发给 GLM 的那一条请求里、开放问答打开时、按白名单子集提供。开放问答关闭或非儿童模式时，没有任何模型能看到它。

## 5. 部署：用户要做的事

在 VM 的 `deploy/providers.env` 里加（`chmod 600`，只写变量名和格式，值由用户自己填）：

```text
KID_PET_GLM_API_KEY=<智谱开放平台 API Key，形如 xxxxxxxx.yyyyyyyy>
KID_PET_GLM_MODEL=glm-4.7-flash
```

- `KID_PET_GLM_MODEL` 可以不写，默认就是 `glm-4.7-flash`。核实情况：搜到的公开资料都说智谱目前的免费 Flash 模型是 `glm-4.7-flash`（据报道 `glm-4.5-flash` 于 2026-01-30 下线并转到它）；`glm-4-flash` 现在还能不能用没有查到确切说法。写入前请到 bigmodel.cn 控制台的模型列表确认一下。对 `glm-4.5`/`4.6`/`4.7`/`5` 系列，请求里会自动带上 `thinking: disabled`。
- compose 现在让 pet-learning 也读 `providers.env`（xiaozhi-server 本来就读）。
- 然后 `docker compose --env-file .env up -d --build`。在面板“开放问答”页确认开关不再置灰，勾好动作，再输入密码 + TOTP 打开。

## 6. 真机验证计划

详细步骤见 PR 描述。回归工具是 `tools/voice_probe.py`，它在 #113 里，那个 PR 还没合并。

1. 部署这个分支（用户操作），确认 `docker compose logs xiaozhi-server` 里有 `CHILD_GUARD_INSTALLED`、checklist 通过。
2. 开放问答**关闭**时：用 voice_probe 跑 #114 的固定命令集，结果应和 #114 一致；随便问一句开放问题，应该回固定兜底句，pet-learning 日志里没有 `open_chat`。
3. 在面板上加密钥、打开开放问答，然后用 voice_probe 问几句开放问题，检查回答简短、没有表情和符号、`voice_turn kind=open_chat`。
4. 说“我们玩点什么吧”之类的话：应看到 `reason=open_chat_tool`，板子日志有 `PET_VOICE_MCP name=self_pet_request result=`，播报和设备结果一致。在安静时段、低电量下再试，应听到对应的拒绝句。
5. 说“我困了想睡觉”：默认子集里没有睡觉，板子不应进入睡觉流程。
6. 熔断：临时把密钥改错，连问 3 次，面板应显示“连续出错 3 次，已自动关闭”，审计里有 `open_chat.breaker`。
7. 静默 120 s：连接应静默结束，不播结束语。
