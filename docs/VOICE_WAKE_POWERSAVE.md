# 唤醒与省电：语音基线 C3 / C6 / U1 / U2 结论

依据：`xiaozhi-pet-backups/voice/baseline-report.md`（2026-11-09 真机基线）。
上游锁定 v2.5.2（`49b4380`）；本机核对用的是 2.5.2 源码副本（无 `.git`，
未能核对 commit，函数体与基线报告所引一致）。

## 根因结论

**U1（上游 PowerSaveTimer 关掉唤醒词）在本板不成立，不是 idle 叫不醒的根因。**

1. 构造参数：`waveshare-s3-epaper-3.97.cc:66`
   `new PowerSaveTimer(-1, 100, 300)`，即 `cpu_max_freq=-1`、
   `seconds_to_sleep=100`、`seconds_to_shutdown=300`。
2. `PowerSaveCheck()`（`power_save_timer.cc`）里关唤醒词、关音频输入、
   `esp_pm_configure(light_sleep_enable=true)` 三步都包在
   `if (cpu_max_freq_ != -1)` 里，本板为 -1，全部不执行。
   本板也没有调用 `OnEnterSleepMode`，进入睡眠只会置 `in_sleep_mode_`。
   另外工程没有开 `CONFIG_PM_ENABLE`，`esp_pm_configure` 本来也不会生效。
3. 实机日志 `boot-log-*.txt:53`：
   `PowerSaveTimer: Power save timer is disabled by settings`
   （NVS `wifi/sleep_mode=false`）。全部基线串口里没有一行
   `Enabling power save mode`，计时器根本没有跑。
4. kid_app 的 `PET_POWER idle/nap` 只改画面：`power_manager.h` 头注释
   "Idle and Nap only change what the pet does on screen"，
   `HandlePowerEvent`（`p1_integrate.py`）里不碰音频、AFE、CPU 频率。
   触发顺序：kid_app Idle 在 60 s 无输入后，Nap 在 10 min 后；
   上游 sleep 在 100 s（实际被禁用）。二者互不调用。
5. 基线矛盾的解释：既然 idle/nap 都不会停 AFE，"idle 5 s 命中、nap 命中"
   与源码一致；idle 约 40 次 0 命中不能用省电解释。串口里 day1/2/4 整轮
   没有任何 `Wake word detected`，这三轮用的是旧版工具（每句重新唤醒）、
   音量也曾从 18% 调到 46%，更像是**放音链路/电平问题**，需要再测
   （见下面验证步骤）。本 PR 不声称已经修好 idle 叫不醒；它做的是
   把"省电不会停唤醒"变成合约，并给出下次判断用的日志。
6. C6：会话 2–3 分钟自动结束是**服务端**设置的
   （xiaozhi-server `no_voice_close_connect`，配置项
   `close_connection_no_voice_time`，上游默认 120 s；日志
   `结束对话，无需发送结束提示语`，随后 `State: listening -> idle`）。
   设备端没有这个计时器。结束时服务端不发提示。

## 改了什么（overlay，`scripts/p1_integrate.py`）

- `patch_power_save_timer`：`PowerSaveTimer(-1, 100, 300)` →
  `(-1, -1, 300)`。去掉 sleep 步，将来即使有人给 `cpu_max_freq` 赋值，
  也不会停 AFE / 开轻睡眠。300 s 关机接管逻辑不变。
- `UpdateWakeGuard`（pet_input 每圈）：
  - 板子 idle 且 `IsWakeWordRunning()==false` 时重新
    `EnableWakeWordDetection(true)`，5 s 一次，最多 6 次，睡眠流程中不动。
  - 日志 `PET_WAKE_GUARD sleep=<0|1> wakeword=<0|1> input=<0|1> power=<state>
    state=<n> why=<enter_idle|enter_nap|periodic|chat_end>`；进入 idle/nap
    各打一次，idle 时每 30 s 一次；修复时 `PET_WAKE_GUARD repair=1 try=n`。
    `sleep=1` 表示 kid_app 处于 Idle 或 Nap。
- C6 提示音：会话从 listening/speaking 回 idle 时播一声内置
  `OGG_VIBRATION`（不新增 TTS、不新增音频文件），夜间（Night）和睡觉流程中
  不播；日志 `PET_CHAT_END_CUE`。
- 没有降频、没有开轻睡眠、没有改屏幕刷新策略（见耗电）。

## 服务端建议值（本 PR 不改服务端）

- `close_connection_no_voice_time`：120 s 对孩子偏短，建议 **180 s**
  （孩子想一想、翻书、等大人的时间）；再长会让麦克风长时间保持上传
  与屏幕 listening 状态。
- 保持"结束时不送上游结束提示词"（child_voice_guard 已守住）；
  设备端的提示音就是结束提示。

## 耗电估算（估算，非实测）

- 本 PR 对待机电流**基本无变化**：当前固件的省电计时器本来就是禁用的，
  AFE 一直在跑；这里只是把这种状态固定下来并加日志。
- 为什么不降频/轻睡眠：ESP32-S3 数据手册给出的量级：CPU 240 MHz 全速约
  30–50 mA，Wi-Fi modem-sleep 平均约 15–30 mA 量级，light-sleep 约
  0.1–0.3 mA 量级。esp-sr 的 WakeNet9 需要持续取麦克风帧并跑神经网络，
  占用约一个核的可观比例，轻睡眠下无法运行（CPU 与 I2S 时钟停转）。
  若改用"idle 轻睡眠"，待机可从几十 mA 降到个位数 mA，但唤醒词就失效，
  与需求相反。
- 续航影响量级：以 1000 mAh 电池、待机平均 60–80 mA 估，AFE 常开时
  纯待机约 12–16 小时；这与现状一致，没有恶化。要进一步省电，
  应在**无人时段**（Night/Nap 且低电量，PowerManager 已有 Off）关机，
  而不是靠轻睡眠。电池实际容量与 PMIC 电流需要在板上用电流表实测，
  上面数字只用于量级判断。

## 刷机后验证

1. 串口过滤 `PET_WAKE_GUARD`：idle 时应看到
   `sleep=1 wakeword=1 input=1`（`why=enter_idle`，之后每 30 s 一行）；
   出现 `wakeword=0` 则会跟着 `repair=1` 行，说明有东西关过唤醒词。
2. nap 同样检查 `why=enter_nap` 行。
3. idle 重复唤醒：用 `tools/voice_probe.py wake`，**先**用 `noise` 与
   扬声器电平确认放音够大（基线中曾有 18% 作废），至少 10 次、
   间隔 ≥200 s（U2）。命中率仍低而 `wakeword=1`，则问题在放音/麦克风/
   模型灵敏度（阈值），不在省电。
4. 开一次会话后不说话等待服务端超时，串口应出现
   `State: listening -> idle` 与 `PET_CHAT_END_CUE`，并听到一声提示音。
