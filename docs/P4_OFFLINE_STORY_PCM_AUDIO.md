# P4 离线绘本故事语音：通过小智原有输出任务播放

## 核心约束与实现

**绝不从 Pet/SD 任务直接调用 ES8311 底层 I2S。** 由于 XiaoZhi v2.5.2 原有 `AudioService::AudioOutputTask` 是唯一的扬声器输出者，本功能对锁定的上游源码增加以下受控接口：

- `BeginLocalStoryPcm(session)`：仅当设备已初始化、codec 输出采样率为 24 kHz 且没有正在处理的服务器语音时开始当前书页的会话。
- `TryQueueLocalStoryPcm(session, pcm, samples)`：最多提交 480 个采样点（20 ms），进入原来 `audio_playback_queue_`（最多两个待播放块）。不允许 SDK 音频解码与该包混合。
- `CancelLocalStoryPcm()`：将当前故事 session 标记失效。原来音频输出任务逐帧检测 session 是否还有效，丢弃已经翻页或退出的陈旧音频帧。
- `Application::SetDeviceState`：一旦状态切换离开 Idle，就先失效本地故事音频，优先保证小智正常语音/听写。

实际板卡的 `PetRender` 只有成功通过 CRC 校验并将书页完整刷到 EPD 后，才把该页码传给单独的 `FreertosStoryPcmWorker`。其自身另开只读的 KPB1 文件句柄，与显示任务互不抢占 `fseek`；以 480 采样/次读取 24kHz PCM，尝试送进 `AudioService`，队列满时短暂退避、停止/翻页时丢弃旧会话。

回到宠物主页或退出 Pet 模式时，发送停止请求并失效会话；不得把之前的故事声音继续播放到普通小智界面。声音回调永远不访问 NVS、LVGL、EPD 或底层 I2S。

## 代码位置

- `scripts/p4_audio_queue_patch.py`：基于固定 upstream commit 的单一锚点补丁；由 `scripts/p1_integrate.py` 在 disposable checkout 实际应用。
- `firmware/components/kid_app/src/freertos_story_pcm_worker.cc`：低优先级、无硬件音频驱动依赖的 24kHz PCM 流式任务。
- `tests/test_p4_story_audio_contract.py`：验证 AudioOutputTask 仍然唯一写 codec、session 失效机制、实体板卡 EPD 成功后才提交播报。
- `p1-integrated-firmware.yml`：在 CI 的 ESP-IDF 6.1 容器内用 Waveshare 官方板级代码做真正的固件编译。

## 已知边界

此代码实现了实际板卡音频路径的**软件接线**，不是完成的真机验收。实际扬声器的声量、是否产生可闻失真、翻页后残留的一帧声音、话筒唤醒/联网对话抢占体验，必须通过 Waveshare 板和 ES8311 进行测试。

PCM 24 kHz 单声道 16 位、每页最长 60 秒；不对未经家长审核的音频进行播报。插入未知 TF 卡不能证明内容安全；CRC 不是数字签名。当前固定书名 `forest_friends`，不自动扫描/运行任意文件，也没有引入新的网络音频权限。

不会自动刷写固件、操作设备或启用 OTA；提交仅更新源码与编译产物。
