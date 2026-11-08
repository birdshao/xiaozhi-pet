# V6-02 语音固定命令（设备端）

服务端的固定命令匹配器（S5，见 [SERVER_S5](SERVER_S5.md) 与 [V5/V6 服务端方案](V5_V6_SERVER_PLAN.md) §6.2）已实现。本文档管**设备端**：收到一条固定命令后，团团怎么做，设备端接口（命令名、MCP 工具），以及守卫怎么把它送到设备。

## 路径

```text
孩子说话 → 小智服务端 ASR → 守卫调 /v1/voice/{id}/turn → 固定命令匹配器
        → local_command（枚举名，带签名）
        → 守卫经小智服务端调设备端 MCP 工具 self.pet.request({"action": 名字})
        → 板端白名单 → RequestVoiceCommand(name) → pet_input 任务 → DecideVoiceCommand
        → 和按键同一条代码路径 → 结果 {"action","result"} 回到守卫 → 按结果选一句话播报
```

原则（ADR-008）：程序管动作，模型管语言。只有确定性的固定命令匹配器能提议动作；MCP 只承载"提议"，执不执行由板上 PetCore 的确定性规则决定。语音不写记忆（`MemorySource::Voice` 仍被拒绝）、不写照顾/成长、不额外加分。

## MCP 工具 `self.pet.request(action)`

已在锁定版本上核对（固件 v2.5.2 `49b4380`、服务端 v0.9.8 `8f2dda09`）：

| 环节 | 锁定版本里的做法 | 本项目 |
|---|---|---|
| 设备注册 | `McpServer::GetInstance().AddTool(name, 描述, PropertyList, 回调)`；板子在构造时注册，`Application` 之后加的公共工具排在前面、板子工具保留 | `StartPetInput()` 起好 `pet_input` 任务后调 `InitializePetTools()` 注册；任务没起来就不注册（守卫随之回"用按键"） |
| 服务端看到的工具 | 设备 hello 带 `features.mcp` 时服务端建 `MCPClient`，发 `tools/list`（不带 `withUserTools`，所以 `AddUserOnlyTool` 的工具服务端看不到）；名字经 `sanitize_tool_name` 变成 `self_pet_request` | 用普通 `AddTool` |
| 调用 | `core.providers.tools.device_mcp.mcp_handler.call_mcp_tool(conn, conn.mcp_client, "self_pet_request", {"action": X}, timeout)` 发 `tools/call`，等设备回包；`isError` 抛异常，正常时返回 `content[0].text` | 守卫 3 s 超时 |
| 设备执行 | `DoToolCall` 先按 `PropertyList` 校验类型，再 `Application::Schedule` 到**主任务**执行回调 | 回调只做白名单 + 投递 + 最多等 400 ms（`kVoiceReplyWaitMs`）`pet_input` 的结果；上游拍照工具也在主任务阻塞，量级可接受 |

- 参数：`action`，字符串，最长 15。**只接受**服务端 `intent.py` 的 `ACTIONS`（13 个：`SLEEP_REQUEST PAGE_NEXT PAGE_PREV OPEN_READER OPEN_GAME PET_FEED PET_GROOM GO_HOME ASK_NEEDS PLAY_SONG TELL_JOKE START_BRUSH START_WASH`），C++ 白名单 `IsVoiceRequestAction` 与之由合约测试保持一致。`TELL_TIME`/`TELL_BATTERY` 只给自测用，工具不收；其他值返回 MCP 错误，不会到达宠物。
- 返回：`{"action":"PET_FEED","result":"done"}`。`result` 就是下面 `PET_VOICE_CMD` 的结果词，外加 `pending`（400 ms 内 `pet_input` 没答，命令仍会执行）。
- 工具回调不碰记忆、照顾、账本、宠物动作（合约测试断言），只调 `RequestVoiceCommand`。
- 日志：`PET_VOICE_MCP name=<NAME> result=<token>`；被拒的只记长度 `PET_VOICE_MCP result=refused len=<n>`。

已知边界：非儿童模式（`KID_PET_CHILD_MODE` 不是 1）下小智的大模型也能看到这个工具。儿童模式下守卫吃掉每一句 ASR，大模型不运行；而且即使被调用，设备照样按同一套规则决定。

## 守卫怎么播报

守卫收到带签名的 `local_command` 后，**在单独的任务里**调设备工具（手动拾音模式下 ASR 是在 websocket 接收循环里被 await 的，设备回包也走这个循环，所以不能就地等），按设备结果选话术：

| 设备结果 | 守卫说 |
|---|---|
| `done` | 服务端审核过的固定句（`tts_text`）；为空或过长时"好的。" |
| `deferred` | 同上，并设 `conn.close_after_chat`：这句说完小智断开会话，设备回到空闲后打开画面 |
| `already` | "我们已经在这里啦。" |
| `quiet` | "现在是安静时间，我们等一会儿再玩吧。" |
| `low_battery` | "团团有点没电了，请大人帮忙充上电吧。" |
| `not_in_reader` | "我们先打开绘本，再来翻页吧。" |
| `busy`、`no_book`、`rejected`、`off`、`parent`、`ignored`、`expired`、`no_clock`、`no_battery` | "现在还不行哦，我们等一会儿再试吧。" |
| `pending` | "好的，我们试试看。"，并设 `close_after_chat`（板子可能已把它延后） |
| 没有 MCP / 没有这个工具 / 超时 / 报错 / 回包不对（不是 JSON、动作名不一致、未知结果词） | 原来的"我听到了，我们用设备上的按键完成这个动作吧。" |

`action` 不是 `[A-Z_]{1,15}` 的签名回复按服务异常处理（`NO_SERVICE`），不会调设备。

## 命令名（与服务端方案一致）

| 命令名 | 说法示例 | 设备行为 |
|---|---|---|
| `SLEEP_REQUEST` | 团团睡觉 | 走 V2-09 睡觉流程：睡觉画面 + 3 s 取消窗口，按任意拨轮/BOOT 键取消 |
| `TELL_TIME` | 几点了 | 在首页挥手并问好，顶部状态带显示时间；数字由服务端 TTS 播报（设备没有数字音频） |
| `TELL_BATTERY` | 还有多少电 | 同上，日志给出电量百分比与充电状态 |
| `PAGE_NEXT` | 下一页 | 只在读书器里翻页（= 拨轮下）；不在读书器里则温和提示（问好），不动作 |
| `PAGE_PREV` | 上一页 | 只在读书器里往回翻（= 拨轮上）；其余同 `PAGE_NEXT` |
| `OPEN_READER` | 讲故事 | 和首页"讲故事"同一路径（`StartStory`）；没有书时显示 #75 的无书提示 |
| `GO_HOME` | 回首页 | = 拨轮长按：读书器回首页（书在读 → 读书器的"回家"，书没打开 → `StopStory`），游戏/照顾/声音活动里交给该活动的长按（回首页） |
| `PET_FEED` | 喂团团 | 打开**选菜**画面（= 照顾菜单第一张"喂食"卡，`StartCare(CareKind::Feed)`）；孩子用拨轮选菜、按下才喂。语音本身不发 `Feed`、不写照顾记录 |
| `PET_GROOM` | 给团团梳毛 | 打开**梳毛**（= 照顾菜单"梳毛"卡，`StartCare(CareKind::Brush)`）；拨轮来回转才算 |

`TELL_TIME` / `TELL_BATTERY` 在服务端方案 §6.2 里是服务端用快照直接回答的；设备端额外认这两个名字，用于自测和以后改为设备本地回答（MCP 工具不收它们）。

### 为什么"喂团团"是打开选菜，而不是直接喂

- V8 的喂食是"首页 → 照顾菜单 → 选食物"，选菜事件 `Fed` 只认拨轮来源（[V8_CARE_AND_GROWTH](V8_CARE_AND_GROWTH.md) §3.9、ADR-012），语音、网络一律拒绝。
- 直接喂（V2 的 `PetAction::Feed`）会计入记忆的"今天喂了几次"（`MemoryStore::NotePetAction`），等于语音写记忆；V6-02 的合约测试一直断言语音分发里没有 `PetAction::Feed`。
- V8 §2 里"语音进行中首页喂食退回直接喂食"说的是**按拨轮**时恰好在说话；语音命令本身只提议，所以选菜画面等对话结束再打开（见下），由孩子按下完成。
- 因此服务端这条命令的固定句改为"好呀，选一样好吃的喂团团吧。"（`intent.py`）。

### 画面类活动等对话结束（`deferred`）

小智不在空闲状态时（听/说），`UpdateGame` 会立刻结束任何游戏/菜单（`PET_GAME_END reason=voice`），而语音命令必然在对话中到达。所以**打开画面的提议**——`OPEN_GAME`、`PLAY_SONG`、`START_BRUSH`、`START_WASH`、`PET_FEED`、`PET_GROOM`——在 `chat_active` 时返回 `result=deferred`：板子记下这条命令（一个槽，新命令覆盖），守卫说完那句并让小智断开会话；设备回到空闲后**按当时的情况重新判断一次**（场景、安静时段、电量都重查），通过就打开；20 s（`kVoiceDeferMs`，从判断时算起；守卫的固定句都很短）内没等到空闲则丢弃，日志 `result=expired`。重新判断若被拒（例如这期间进入安静时段或电量变低），团团照常用问好/挥手提示，但守卫那句"好呀"已经说过，不会再纠正——已知限制。拒绝（忙、安静、低电）当场就说，不延后。其他命令（翻页、回首页、讲故事、睡觉、几点、电量、笑话、饿不饿）在对话中立即执行。

## 各场景的行为

纯 C++：`kid_app/voice_command.{h,cc}`（`DecideVoiceCommand`，无状态、无时钟、无 IO），主机单测 `tests/cpp/test_voice_command.cc`。

| 命令 | 首页 | 读书 | 游戏（含照顾、声音活动） | 安静时段 | 低电量 | 对话中 | 睡觉中 / 家长菜单 / 非宠物模式 |
|---|---|---|---|---|---|---|---|
| `SLEEP_REQUEST` | 执行 | 执行 | 执行 | 执行 | 执行 | 执行 | 已在睡：忽略（`already`）；家长菜单、宠物模式关闭：忽略 |
| `TELL_TIME` | 显示+问好；时钟未知 → `no_clock` 提示 | `busy` 提示 | `busy` 提示 | 只显示，不出声 | 照常 | 执行 | 忽略 |
| `TELL_BATTERY` | 显示+问好；无 PMIC 读数 → `no_battery` | `busy` | `busy` | 只显示，不出声 | 照常 | 执行 | 忽略 |
| `PAGE_NEXT` | `not_in_reader` 提示 | 翻页（正在回答问题时 `busy`，无书时 `no_book`） | `not_in_reader` | 照常 | 照常 | 执行 | 忽略 |
| `PAGE_PREV` | `not_in_reader` 提示 | 往回翻（同上：`busy`/`no_book`） | `not_in_reader` | 照常 | 照常 | 执行 | 忽略 |
| `OPEN_READER` | 执行 | `already`（不重复） | `busy` 提示 | 拒绝，`quiet` | 照常（阅读很安静） | 执行 | 忽略 |
| `GO_HOME` | `already` | 回首页 | 回首页（活动自己的长按） | 照常 | 照常 | 执行 | 忽略 |
| `PET_FEED` | 打开选菜 | `busy` 提示 | `busy` 提示 | 拒绝，`quiet` | 拒绝，`low_battery` | `deferred` | 忽略 |
| `PET_GROOM` | 打开梳毛 | `busy` 提示 | `busy` 提示 | 拒绝，`quiet` | 拒绝，`low_battery` | `deferred` | 忽略 |
| `OPEN_GAME` 等 V8 命令 | 见 [V8_SOUND_AND_ROUTINES](V8_SOUND_AND_ROUTINES.md) §4 | | | | | 画面类 `deferred` | |
| 未知名字 | 全部场景：忽略，`result=unknown`，不动作、不提示 |

"提示"= 团团问好（和首页被拒绝的"游戏"一样），不是报错；安静时段的拒绝不发声。命令到达时若正在小睡/晨间窗口，先像拨轮一样唤醒再处理。注意小智在说话时游戏已被结束（`UpdateGame` 在同一轮先跑），所以对话中说"回首页"多半得到 `already`，守卫说"我们已经在这里啦。"；游戏里的 `GO_HOME` 分支实际只有自测会走到。非儿童模式下大模型也能看到 `self_pet_request`，同一守卫任务未做并发去重（连续两句命令可能先后各说一句），记为后续。

## 日志与自测

- 每条命令一行：`PET_VOICE_CMD name=<NAME> result=<token> action=<..> hint=<..> scene=<..> proposal=<..> later=<0|1>`。`result`：`done`、`deferred`、`unknown`、`off`、`parent`、`already`、`ignored`、`busy`、`not_in_reader`、`quiet`、`no_clock`、`no_battery`、`low_battery`、`rejected`（宠物控制器拒绝）、`no_book`；延后的命令过期时 `PET_VOICE_CMD name=<NAME> result=expired`。`later=1` 表示这是对话结束后重新判断的那一次。
- 另有 `PET_VOICE_TIME hour= minute=`、`PET_VOICE_BATTERY pct= charging= state=`、`PET_VOICE_MCP name= result=`。
- PETSELFTEST1 步骤 `vcmd <NAME>`：名字由 `[A-Za-z0-9_]` 组成，不超过 15 个字符，不含空格；未知名字也会被注入（用来验证被忽略）。`vcmd` 直接调 `RequestVoiceCommand`（不经 MCP 白名单），自测时小智空闲，所以不会 `deferred`。`SLEEP_REQUEST` 会启动睡觉流程，场景里必须用按键取消或先 `alarm`；`tools/selftest_report.py plan` 会把没有后续按键的 `vcmd SLEEP_REQUEST` 估算为会关机。
- 场景 `tools/selftest/v6_voice_commands.pettest`（约 70 s 脚本，`plan` 估算监视 93 s）：首页下一页 → 未知命令 → 几点了 → 注入电量 80 % 后问电量 → 讲故事 → 读书中问电量（`busy`）→ 下一页 → 回首页 → 先 `alarm 30` 再 `vcmd SLEEP_REQUEST`，1.5 s 后按键取消（不关机）。
- 场景 `tools/selftest/v6_voice_commands_more.pettest`（90 s 脚本，`plan` 估算监视 113 s，`phase day`）：首页上一页（`not_in_reader`）→ 首页回首页（`already`）→ 讲故事 → 下一页 → 上一页 → 回首页 → 喂团团（选菜画面，拨轮长按离开，不喂、无照顾记录）→ 梳毛 → 回首页 → 玩游戏 → 回首页。
- 真正说话、服务端意图、MCP 往返和 TTS 播报要在真机上用 voice-harness（电脑放音）回归，见 PR 描述里的真机验证计划。

## V8 增补

`ASK_NEEDS`、`OPEN_GAME`、`PLAY_SONG`、`TELL_JOKE`、`START_BRUSH`、`START_WASH` 已进入设备端分发表（"晚安"由服务端映射为 `SLEEP_REQUEST`，无单独命令名）（以 `VoiceProposal` 形式提议，不改 `VoiceAction`），场景行为表见 [V8_SOUND_AND_ROUTINES.md](V8_SOUND_AND_ROUTINES.md) §4。其中打开画面的几个在对话中返回 `deferred`（见上）。`PAGE_PREV`、`PET_FEED`、`PET_GROOM`、`GO_HOME` 现在也已分发（本文上表）。
