# 唤醒词：你好团团（MultiNet）与回退

默认唤醒词是"你好团团"，用 esp-sr MultiNet7 的自定义唤醒词实现，不改小智 C++ 代码，只改构建配置（`scripts/p1_integrate.py` 的 `WAKE_WORD_PROFILES`）。这是**在一块板子上的试验**，验收达标前不当作定版。

## 切换与回退

| 方式 | 用法 |
|---|---|
| 环境变量 | `PET_WAKE_WORD=xiaozhi python scripts/p1_integrate.py --source ...` |
| 命令行 | `p1_integrate.py --wake-word xiaozhi`（优先于环境变量） |
| 改默认 | 把 `WAKE_WORD_DEFAULT` 改成 `"xiaozhi"` |
| CI | 手动触发 `p1-integrated-firmware` 工作流，输入 `wake_word=xiaozhi` |

`xiaozhi` 不覆盖任何唤醒词配置，回到上游默认（WakeNet9 `wn9_nihaoxiaozhi_tts`）。服务端 `deploy/xiaozhi/child.config.yaml` 的 `wakeup_words` 两个词都保留，所以回退时服务端不用动。刷回旧词需要重新编译并刷机（只刷 app 分区即可；assets 分区里的 srmodels 随固件重新打包，需按当时的刷机流程确认）。

## 写进 config.json 的配置

```
CONFIG_USE_AFE_WAKE_WORD=n
CONFIG_USE_CUSTOM_WAKE_WORD=y
CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=n      # 上游 sdkconfig.defaults.esp32s3 默认打开，不让它进 srmodels
CONFIG_SR_MN_CN_MULTINET7_QUANT=y
CONFIG_CUSTOM_WAKE_WORD="ni hao tuan tuan"
CONFIG_CUSTOM_WAKE_WORD_DISPLAY="你好团团"
CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20      # 1-99，越小越灵敏
```

上游两种检测器互斥：srmodels 里有 MultiNet 就走 MultiNet，否则走 WakeNet。

## srmodels 空间

本工程分区表（`partitions/v2/16m.csv`）没有单独的 srmodels 分区，`srmodels.bin` 在 `assets` 分区（spiffs，8 MiB）里。

| 项 | 大小 |
|---|---|
| assets 分区 | 8192 KB |
| 现在已用（启动日志，含 WakeNet） | 2880 KB |
| 减 wn9_nihaoxiaozhi_tts | −289 KB |
| 加 mn7_cn（esp-sr 主干目录实测 2617 KB） | +2617 KB |
| 换后约 | 5208 KB，剩 ≈ 2984 KB |

`tests/test_p1_wake_word.py` 断言上面的算术（余量 ≥ 1 MiB），CI 里再对真实分区表和下载到的 mn7_cn 目录核对。注意：mn7_cn 大小测自 esp-sr 主干，锁定版本是 2.4.7；CI 的真实核对会暴露差异。

## PSRAM 预算（8 MiB，`esp_psram` 日志确认）

PSRAM 剩余量**没有被固件记录过**（启动日志只有 SRAM），下面是按源码核算的估计，换词后必须上板看实测。

| 项 | 估计 | 依据 |
|---|---|---|
| 墨水屏 LVGL 全屏缓冲 `buffer_1` | 750 KB | 800×480×RGB565，`custom_lcd_display.cc` |
| 墨水屏驱动 `buffer` | ≤ 190 KB | `epd_spi_data.buffer_len`，未读到确切值 |
| LVGL 内置内存池 | 1024 KB | `CONFIG_LV_MEM_SIZE_KILOBYTES=1024`，在 PSRAM。额外 2.5 MiB 池和 2 MiB 图像缓存只在 `SpiLcdDisplay` 构造里，本板 `CustomEpdDisplay` 走 `LcdDisplay` 基类，不触发 |
| 唤醒音频缓存 | 62.5 KB | 启动日志 `WakeWordCache: Allocated 64000 bytes` |
| WakeNet9 + AFE | ≈ 330–500 KB | 官方 benchmark 324 KB，AFE 为 `ALLOC_MORE_PSRAM` |
| 音频编解码、网络、任务栈等 | ≈ 0.5–1 MB | 估计 |
| kid_app 常驻 | ≈ 0.1 MB | 同步缓冲 ≈ 18 KB + 结构体；单帧 48 KB 按需分配 |
| kid_app 瞬时峰值 | ≤ 192 KB | 灰度书页 192 KB；鹦鹉录音 160 KB；两者不同时出现 |
| **合计（换前）** | **≈ 3.1–3.9 MB** | |
| 换 MultiNet7（去掉 WakeNet） | +2.6 MB（官方 2920 KB − WakeNet 约 324 KB） | |
| **合计（换后）** | **≈ 5.7–6.5 MB / 8 MB** | 余量约 1.5–2.3 MB |

结论：估计装得下，但不是宽裕，且没有实测。上板后必须记录：PSRAM 空闲和最低空闲、`After AFE create` 内部 RAM（现在最低 33468 B，很紧；MultiNet 官方内部 RAM 约 18 KB，WakeNet 约 16 KB，应持平）。MultiNet 每 32 ms 帧约 11 ms（WakeNet 约 3 ms），多占约 1/3 个核，可能影响电子纸刷新、音频、功耗。

## 空闲/节能逻辑是否依赖 WakeNet

- 在 `firmware/`、`scripts/p1_integrate.py`、`tools/`、`services/` 里没有任何地方引用唤醒词、WakeNet、MultiNet 或"你好小智"。睡眠、小睡、晨间窗口、闹钟开机的守卫只看 `kDeviceStateIdle`、`sleep_active_`、`napping_` 等设备状态，不看唤醒词类型。
- 睡眠"熟睡"是 PMIC 真关机，闹钟开机走 RTC，都不靠语音；浅睡眠的唤醒源是拨轮/BOOT/PWR/姿态，不含唤醒词。所以休眠唤醒不受影响。
- 上游 `application.cc` 用 `IsAfeWakeWord()`（= 引擎有唤醒词，WakeNet 和 MultiNet 都为真）决定各状态下开关检测；`WAKE_WORD_DETECTION_IN_LISTENING` 默认关。换检测器后这些切换点行为一致。
- 一个例外是功耗：常开检测的 CPU 负载变大，**电池续航需要实测**。Idle 状态本来是否真进浅睡眠，要看实际功耗，不能假设。

## 显示文字与"嗨我在"

- `CUSTOM_WAKE_WORD_DISPLAY="你好团团"` 只用于唤醒后作为"检测到唤醒词"的文本发给服务器（`SendWakeWordDetected`）。板子界面不显示这个字符串；仓库里没有"嗨我在"。
- 服务器对唤醒词的处理取决于 `wakeup_words` 列表。上游默认只含"你好小智"等，**不含"你好团团"**。`enable_greeting: false` 时，列表里的词被静默吞掉（没有欢迎语）；**不在列表里的文本会被当作普通话语交给 LLM**，团团会回一句话。所以 `child.config.yaml` 新增了 `wakeup_words: [你好团团, 你好小智]`，并有测试检查它与 `WAKE_WORD_DISPLAY` 一致。
- 欢迎语是服务端行为：`enable_greeting` 关，所以换词前后都没有欢迎语；`wakeup_words_response_cache`（"我一直都在呢"之类缓存应答）只在 ASR 文本路径的 `checkWakeupWords` 里触发，唤醒词的 `detect` 消息不走它。
- 部署新服务端配置需要重启 xiaozhi-server 容器；这次没有部署。

## 风险

1. MultiNet 做常开唤醒，官方没有误唤醒数据，倾向比专用 WakeNet 差，必须按验收门槛实测。
2. PSRAM 余量估计 1.5–2.3 MB，未实测。
3. 内部 RAM 余量本来就紧（AFE 创建后最低约 33 KB）。
4. MultiNet 初始化失败（拼音被拒、内存不足）会静默变成没有唤醒词，只能靠按键；上板看启动日志有无 `Custom wake word` 初始化错误。
5. 合成声测试偏乐观，需要真人儿童录音补测。
6. 服务端 `wakeup_words` 未部署时，"你好团团"会被当作聊天内容。
7. 4 音节"你好团团"与"你好小智"同结构，近音词（团子、桃桃）要靠实测阈值。

## 验收门槛

- 安静环境 1 米命中率 ≥ 90%。
- 3 米或有背景音命中率 ≥ 75%。
- 误唤醒每天 ≤ 1 次。
- 新词不得比"你好小智"基线明显更差。

达不到就回退到"你好小智"（见上），不要靠继续调低阈值硬撑。
