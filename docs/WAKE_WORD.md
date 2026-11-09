# 唤醒词：你好团团（MultiNet）与回退

默认唤醒词是"你好团团"，用 esp-sr MultiNet7 的自定义唤醒词实现，不改小智 C++ 代码，只改构建配置（`scripts/p1_integrate.py` 的 `WAKE_WORD_PROFILES`）。这是**在一块板子上的试验**，验收达标前不当作定版。

## 重要：唤醒词由 assets 分区决定，只刷 app 不会改变

上游 `afe_audio_engine.cc` 在**运行时**检查 srmodels（在 assets 分区里）有没有 MultiNet 模型来选检测器；`custom_wake_word.cc` 的唤醒词和命令都从 `index.json`（也在 assets 里）读取，`CONFIG_USE_*_WAKE_WORD` 在 C++ 里没有引用，它们只影响打包。所以：

- 只刷新 app：板子行为不变（仍按板上旧 assets 走 WakeNet）。
- 只刷新 assets：连旧 app 也会走 MultiNet。
- 切换唤醒词 = **刷 assets 分区**（0x800000，8 MiB 分区）；app 与它同一次构建配套刷。

## 切换

| 方式 | 用法 |
|---|---|
| 环境变量 | `PET_WAKE_WORD=xiaozhi python scripts/p1_integrate.py --source ...` |
| 命令行 | `p1_integrate.py --wake-word xiaozhi`（优先于环境变量） |
| 改默认 | 把 `WAKE_WORD_DEFAULT` 改成 `"xiaozhi"` |
| CI | 手动触发 `p1-integrated-firmware`，输入 `wake_word=xiaozhi`（大小写、空白会被规整） |

`xiaozhi` 不覆盖任何唤醒词配置，回到上游默认（WakeNet9）。

## 部署与刷机顺序（先服务端，再板子）

1. **服务端**：`child.config.yaml` 是构建镜像时 COPY 进去的，只重启容器不会生效，必须重新构建：
   `docker compose up -d --build xiaozhi-server`。先部署，板子刷了新词之后服务端才不会把"你好团团"当聊天内容。
2. **取构建产物**（同一次 CI 运行）：`pet-ota1-app-only-buildonly`（app + manifest）和 `pet-assets-wakeword-buildonly`（`xiaozhi-pet-assets.bin` + `assets-manifest.json`）。`assets-manifest.json` 写明偏移 `0x800000`、大小、SHA256、`paired_app_sha256`（必须等于 app manifest 的 `image_sha256`）和回退用的官方 assets 大小/SHA。核对 SHA 再继续。
3. **先存档板上当前 assets**（只读）：
   `esptool --chip esp32s3 -p COMx read-flash 0x800000 2900513 assets-before.bin`
4. **只写 assets 分区**，不碰 NVS、otadata、bootloader、分区表：
   `esptool --chip esp32s3 -p COMx write-flash 0x800000 xiaozhi-pet-assets.bin`
   app 按既有 OTA1 流程刷同一次构建的 app。本仓库任何脚本和 CI 都不会刷机。
5. **验收看启动日志三样**：`AfeAudioEngine: After AFE create`；`PET_PSRAM_STATS free=… min_free=…`（启动约 30 s 与 90 s 各一行）；`custom_wake_word` 打印的命令列表里含 `ni hao tuan tuan`。缺任一项就不要往下测。

## 回退（两步）

assets 一旦换成 mn7_cn，旧固件也会走 MultiNet，所以必须先把官方 assets 写回：

1. 从全量备份截出官方 assets（2,900,513 B，SHA256 `7476fd37…2fc3e`，见 `docs/hardware/OFFICIAL_BASELINE_20261025.md`），脚本会校验 SHA：
   `python scripts/restore_official_assets.py --image xiaozhi-pet-backups/full-flash-20261107-104847.bin --out assets-official.bin`
2. `esptool --chip esp32s3 -p COMx write-flash 0x800000 assets-official.bin`，只写这一个分区。
3. 然后看是否需要把 app 也换回 `PET_WAKE_WORD=xiaozhi` 构建的版本（官方 assets 配 WakeNet 构建才一致）。服务端 `wakeup_words` 两个词都保留，不用动。

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

PSRAM 剩余量原先没有被固件记录过。本 PR 的 p1 补丁加了 `PET_PSRAM_STATS` 启动日志（free / min_free / largest / total）。下面是按源码核算的估计，必须用这行日志核对。

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
- 部署新服务端配置需要重新构建镜像（见上），这次没有部署。

## 风险

1. MultiNet 做常开唤醒，官方没有误唤醒数据，倾向比专用 WakeNet 差，必须按验收门槛实测。
2. PSRAM 余量估计 1.5–2.3 MB，未实测。
3. 内部 RAM 余量本来就紧（AFE 创建后最低约 33 KB）。
4. MultiNet 失败有两种，表现不同：(a) 引擎初始化失败（上游 `multinet_->create()` 没有判空，内存不够会直接崩溃；其它初始化失败时，连上行语音处理也不可用）；(b) 拼音被 mn7 拒绝：`esp_mn_commands_update` 的返回值没人检查，结果是永远不唤醒，没有报错。验收时必须在启动日志里看到命令列表含 `ni hao tuan tuan`。
5. 合成声测试偏乐观，需要真人儿童录音补测。
6. 服务端 `wakeup_words` 未重新构建部署时，"你好团团"会被当作聊天内容。
7. 4 音节"你好团团"与"你好小智"同结构，近音词（团子、桃桃）要靠实测阈值。

## 验收门槛

- 安静环境 1 米命中率 ≥ 90%。
- 3 米或有背景音命中率 ≥ 75%。
- 误唤醒每天 ≤ 1 次。
- 新词不得比"你好小智"基线明显更差。

达不到就回退到"你好小智"（见上），不要靠继续调低阈值硬撑。
