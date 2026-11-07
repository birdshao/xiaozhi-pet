# V6-02 语音固定命令（设备端）

服务端的固定命令匹配器（S5，见 [SERVER_S5](SERVER_S5.md) 与 [V5/V6 服务端方案](V5_V6_SERVER_PLAN.md) §6.2）已实现。本文档只管**设备端**：收到一条固定命令后，团团怎么做，以及设备端接口（命令名）。

## 路径

```text
孩子说话 → 小智服务端 ASR → 守卫调 /v1/voice/{id}/turn → 固定命令匹配器
        → local_command（枚举名）→ 设备端 MCP 工具 self.pet.request(action)   ← 后续任务（§11.2）
        → 板端 RequestVoiceCommand(name) → pet_input 任务 → DecideVoiceCommand → 和按键同一条代码路径
```

板端入口 `RequestVoiceCommand(const char* name)`（`scripts/p1_integrate.py` 的 overlay）已经就绪，任何任务都能调用；MCP 工具注册要在锁定的小智版本上核对后另做，在那之前只有自测步骤 `vcmd` 会触发它。语音不写记忆（`MemorySource::Voice` 仍被拒绝），不额外加分；动作成没成功以设备为准。

## 命令名（与服务端方案一致）

| 命令名 | 说法示例 | 设备行为 |
|---|---|---|
| `SLEEP_REQUEST` | 团团睡觉 | 走 V2-09 睡觉流程：睡觉画面 + 3 s 取消窗口，按任意拨轮/BOOT 键取消 |
| `TELL_TIME` | 几点了 | 在首页挥手并问好，顶部状态带显示时间；数字由服务端 TTS 播报（设备没有数字音频） |
| `TELL_BATTERY` | 还有多少电 | 同上，日志给出电量百分比与充电状态 |
| `PAGE_NEXT` | 下一页 | 只在读书器里翻页；不在读书器里则温和提示（问好），不动作 |
| `OPEN_READER` | 讲故事 | 和首页"讲故事"同一路径（`StartStory`）；没有书时显示 #75 的无书提示 |

`TELL_TIME` / `TELL_BATTERY` 在服务端方案 §6.2 里是服务端用快照直接回答的；设备端额外认这两个名字，用于服务端不可用或以后改为设备本地回答。服务端方案里的 `PAGE_PREV`、`OPEN_GAME`、`PET_FEED`、`PET_GROOM`、`GO_HOME` 设备端**还没做**，当作未知命令忽略（日志 `result=unknown`）。

## 各场景的行为

纯 C++：`kid_app/voice_command.{h,cc}`（`DecideVoiceCommand`，无状态、无时钟、无 IO），主机单测 `tests/cpp/test_voice_command.cc`。

| 命令 | 首页 | 读书 | 游戏 | 安静时段 | 低电量 | 睡觉中 / 家长菜单 / 非宠物模式 |
|---|---|---|---|---|---|---|
| `SLEEP_REQUEST` | 执行 | 执行 | 执行 | 执行 | 执行 | 已在睡：忽略（`already`）；家长菜单、宠物模式关闭：忽略 |
| `TELL_TIME` | 显示+问好；时钟未知 → `no_clock` 提示 | `busy` 提示 | `busy` 提示 | 只显示，不出声 | 照常 | 忽略 |
| `TELL_BATTERY` | 显示+问好；无 PMIC 读数 → `no_battery` | `busy` | `busy` | 只显示，不出声 | 照常 | 忽略 |
| `PAGE_NEXT` | `not_in_reader` 提示 | 翻页（正在回答问题时 `busy`，无书时 `no_book`） | `not_in_reader` | 照常 | 照常 | 忽略 |
| `OPEN_READER` | 执行 | `already`（不重复） | `busy` 提示 | 拒绝，`quiet` | 照常（阅读很安静） | 忽略 |
| 未知名字 | 全部场景：忽略，`result=unknown`，不动作、不提示 |

"提示"= 团团问好（和首页被拒绝的"游戏"一样），不是报错；安静时段的拒绝不发声。命令到达时若正在小睡/晨间窗口，先像拨轮一样唤醒再处理。

## 日志与自测

- 每条命令一行：`PET_VOICE_CMD name=<NAME> result=<token> action=<..> hint=<..> scene=<..>`。`result`：`done`、`unknown`、`off`、`parent`、`already`、`ignored`、`busy`、`not_in_reader`、`quiet`、`no_clock`、`no_battery`、`rejected`（宠物控制器拒绝）、`no_book`。
- 另有 `PET_VOICE_TIME hour= minute=`、`PET_VOICE_BATTERY pct= charging= state=`。
- PETSELFTEST1 新步骤 `vcmd <NAME>`：名字由 `[A-Za-z0-9_]` 组成，不超过 15 个字符，不含空格；未知名字也会被注入（用来验证被忽略）。`SLEEP_REQUEST` 会启动睡觉流程，场景里必须用按键取消或先 `alarm`；`tools/selftest_report.py plan` 会把没有后续按键的 `vcmd SLEEP_REQUEST` 估算为会关机。
- 场景 `tools/selftest/v6_voice_commands.pettest`（约 70 s 脚本，`plan` 估算监视 93 s）：首页下一页 → 未知命令 → 几点了 → 注入电量 80 % 后问电量 → 讲故事 → 读书中问电量（`busy`）→ 下一页 → 回首页 → 先 `alarm 30` 再 `vcmd SLEEP_REQUEST`，1.5 s 后按键取消（不关机）。真正说话、服务端意图和 TTS 播报仍需要真机上的人。

## V8 增补

`ASK_NEEDS`、`OPEN_GAME`、`PLAY_SONG`、`GOOD_NIGHT`、`TELL_JOKE`、`START_BRUSH`、`START_WASH` 已进入设备端分发表（以 `VoiceProposal` 形式提议，不改 `VoiceAction`），场景行为表见 [V8_SOUND_AND_ROUTINES.md](V8_SOUND_AND_ROUTINES.md) §4。上文“`OPEN_GAME` 设备端还没做”的说法以该文为准；`PAGE_PREV`、`PET_FEED`、`PET_GROOM`、`GO_HOME` 仍未做。
