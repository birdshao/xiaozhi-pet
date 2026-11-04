# V2 真机试验记录

对应 [开发计划 V2](../DEVELOPMENT_PLAN.md)。从 V2 起，软件逻辑用**脚本化自测**跑，不再需要人按键；只有残影、边框这类只能看的项目由人确认。

## 自动化方法（2026-11-03 起）

- 场景脚本：`tools/selftest/*.pettest`（`PETSELFTEST1` 格式，见 `firmware/components/kid_app/include/kid_app/selftest_script.h`）。
- 主机：`python tools/selftest_report.py sector <场景> <镜像>` 生成 4 KiB 镜像，写到 ota_1 最后一个扇区 `0x7FF000`（固件之后、分区末尾；CI 保证固件不会占到这里）。
- 板子：测试固件从 ota_1 启动时读取该扇区；脚本合法就按步骤把模拟按键注入**与实体按键相同**的队列，否则什么都不做。固件只读不写。
- 试验结束：恢复启动选择器、擦除脚本扇区、`python tools/selftest_report.py report <场景> <日志>` 按场景里的 `# expect` 判定。行为只统计 `mark begin` 之后；无汇总行、崩溃、看门狗、中止一律失败。

## 第 1 轮：首页动作条 + 局部刷新（2026-11-03，自动）

| 项目 | 内容 |
|---|---|
| 固件 | PR #46 构建 [#41628940394](https://github.com/birdshao/xiaozhi-pet/actions/runs/41628940394)，合并提交 `3f986f18`（头提交 `38891cd`），SHA256 `d6a7810c…`，2,940,880 B |
| 场景 | `tools/selftest/v2_home_rail.pettest`，27 步，约 122 s |
| 流程 | 只写 OTA1 并读回 → 写脚本扇区并核对 → 切 OTA1 监控 180 s → 选择器写回并核对 → 擦除脚本扇区并核对 |
| 结果 | **11/11 通过**；`Running partition: ota_1`，无 panic / watchdog |

| 判定项 | 期望 | 实测 |
|---|---|---|
| 选中顺序 | 0,1,2,3,4,3,2,1,0,1,2,3,2,1（到"睡觉"后再往下不动） | 完全一致 |
| 存档次数 | 4（喂食、进绘本、翻页、回首页） | 4 |
| 局部刷新 | 14 次，≤ 2,500 ms | 14 次，**609–611 ms** |
| 全刷 | 4 次，≤ 5,000 ms | 4 次，4,092–4,310 ms |
| 第 9 次移动 | 规划器强制全刷 | n=12 为全刷 |
| 进宠物模式 | 切换确认 | 开机 32 s 进入，一次成功 |

**结论**：局部刷新约 0.6 s，是全刷（约 4.3 s）的 1/7，拨轮移动选中框可以做到"拨一下马上看到"。8 次局刷后自动全刷的逻辑按设计工作。

**人工确认（用户，2026-11-03）**：局刷时没有整屏闪烁；连续局刷后无残影；边框没有变灰。

**过程问题**：第一次判定显示全部失败，原因是 Windows PowerShell 5 的 `Tee-Object` 把日志存成 UTF-16，判定工具按 UTF-8 读不到内容。已让判定工具自动识别编码并补测试；同一份原始日志重判为 PASS。

## 第 2 轮：哄睡关机流程（2026-11-03，自动）

| 项目 | 内容 |
|---|---|
| 固件 | PR #47 构建 [#41641217308](https://github.com/birdshao/xiaozhi-pet/actions/runs/41641217308)，合并提交 `19d8b7ab`（头提交 `280b628`），SHA256 `0e09d769…`，2,943,360 B |
| 场景 | `tools/selftest/v2_home_rail.pettest`（回归，同固件前一版 `f122b98f`：11/11 通过）+ `tools/selftest/v2_sleep.pettest` |
| 流程 | 只写 OTA1 并读回 → 写脚本扇区 → 切 OTA1 监控 200 s → 选择器写回并核对 → 擦除脚本扇区 |
| 结果 | **13/13 通过**；无 panic / watchdog |

| 步骤 | 日志 | 结论 |
|---|---|---|
| 首页选"睡觉"并按下 | `PET_SLEEP_REQUESTED` → 4.3 s 后 `PET_SLEEP_SHOWN` | 熟睡画面全刷 |
| 3 s 内拨动 | `PET_SLEEP_CANCELLED`，回首页 | 取消有效 |
| 按 PWR（注入） | `PET_SLEEP_REQUESTED` → `SHOWN` → 3 s 后 `PET_RTC_ALARM armed_for=21:16:17`、`PET_SLEEP_POWEROFF settled=1 alarm_s=45 armed=1` | 存档已落盘后关机 |
| **USB 供电下关机** | 无 `PET_SLEEP_POWEROFF_RETURNED`，串口断开 | **插着 USB 电源芯片也能真正断电** |
| 45 s 后 | 板子重新上电；随后主机重开串口引起 `rst:0x15`，`PET_DIAG_WAKE ... carried=1`，心跳 `woke_by_alarm=1` | 闹钟准时开机；唤醒原因跨芯片复位保留 |
| 唤醒后 | `PET_SELFTEST_NONE reason=alarm_wake` | 不重复执行脚本 |

**上一轮的问题**（同 PR 前一版固件 `f122b98f`）：闹钟开机后主机重开 USB 串口导致芯片复位，第二次启动已看不到闹钟标志，于是把场景又跑了一遍并再次关机（恢复重试 5 次后成功）。修复：把"闹钟唤醒"连同 RTC 时间记在复位后保留的 RTC 内存里，非上电复位后 60 s 内有效。

**仍待实测**：电池供电下实体 PWR 键能否触发（USB 供电时 GPIO1 无按键边沿，见 V1）。

## 第 3 轮：选项语音播报（2026-11-03，自动）

| 项目 | 内容 |
|---|---|
| 固件 | PR #48 构建 [#41648469825](https://github.com/birdshao/xiaozhi-pet/actions/runs/41648469825)，合并提交 `b7acec7b`（头提交 `39ad8a0`），SHA256 `6a26591f…`，3,094,496 B（含约 150 KB 内嵌提示音） |
| 场景 | `v2_home_rail.pettest`（回归）+ `v2_prompts.pettest` |
| 结果 | 回归 **11/11**；播报 **8/8**；无 panic / watchdog；选择器恢复、脚本扇区擦除 |

| 操作 | 日志 | 结论 |
|---|---|---|
| 停在 2（游戏） | 选中后 360 ms `PET_PROMPT_PLAY index=2`，445 ms 推完 | 播一次 |
| 230 ms 内经过 3、停在 4 | 只有 `index=4` | 快速经过的项不播 |
| 停在 3 | `index=3` | 播一次 |
| 连续经过 2、1，停在 0 | 只有 `index=0` | 只播最后一项 |
| 导航回归每步 2.5 s | 14 次播报全部推完，局刷仍为 609–612 ms | 播报不影响刷新与输入 |

推送耗时约等于片段时长（音频队列只有 2 帧，按播放速度消费），说明流式播放正常。

**待人工确认**：喇叭里的实际声音（每段只响一次、音量、无杂音或截断）。当前是 5 段占位旋律，正式版本应换成家长录音（`tools/make_prompt_audio.py convert`）。
