# P2-06：音频仲裁器策略 v1

2026-10-25，状态：**纯 C++ 状态机已实现并测试；真实 I2S/Codec/小智语音管道尚未接线，不等于扬声器联调通过**。

## 优先级与策略

`AudioCoordinator` 不播放任何 PCM/MP3/Opus 数据，只输出有限的、确定性的控制指令 `AudioPlan`，由后续独立 `AudioPort` 执行。

1. **语音对话 Voice 优先级最高**：开始语音时停止正在播放的故事或宠物提示音；同一会话 token 的重复开始没有新动作；并发不同语音 token 不自动抢断当前语音，避免两个云端请求乱序覆盖。
2. **绘本 Story 次之**：可以取代当前宠物音效；被语音打断时保留故事 token。当前语音正常结束后，发出 `Resume Story`，恢复阅读进度。语音期间请求其他故事暂时拒绝，由上层排队或重试。
3. **宠物提示音 PetCue 最低**：只有音频空闲时才接纳；不缓存过期奖励音效，避免语音结束后突然连续响多条。
4. **安静时段 Quiet 一票否决**：停止当前音频，清空待恢复故事；Quiet 结束也不会自行恢复旧语音或旧音效。
5. **过期完成事件不得污染当前状态**：播放器回调提供原 token，必须匹配当前 stream/token 才被接纳。

一个 `AudioPlan` 至多有两条指令。例如故事 → 语音时是 `Stop Story`、`Start Voice`；语音结束是 `Stop Voice`、`Resume Story`。这可以避免各模块同时直接操作同一个声卡。

## 接线边界

- 本地宠物音效只应由 **PetEventExecutor 事务持久化成功**后的回调触发；NVS 保存失败或重复事件不得发奖音。
- `AudioCoordinator` 应由音频任务单线程拥有；来自云端语音、故事播放器、PetService 的通知通过 FreeRTOS 消息队列转发，不应多线程直接调用。
- `AudioPort` 后续负责真正停播、解码器缓冲清理、淡入淡出、I2S DMA、音量限制、错误处理和资源释放；`AudioPlan` 只表示**意图**，不是音频设备已执行成功的 ACK。
- Token 由实际播放/会话生产者提供：非 0 且跨两个连续会话不可复用，否则迟来的 `Finished` 无法区分旧请求与新请求。
- 与小智原有语音音频任务集成前不要占用或改动官方音频驱动。先在独立测试固件验证抢占和断线恢复；保留已验证的官方 OTA0。

## 已测试

`tests/cpp/test_audio_coordinator.cc`：

- PetCue → Story → Voice 的优先级及双指令输出；
- Voice 完成后恢复前一个 Story；
- 重复 token 和过期音频完成事件的忽略；
- Quiet 强制停播，并禁止自动恢复旧会话。

[实现 PR #18](https://github.com/birdshao/xiaozhi-pet/pull/18) 已合并；[独立 C++ CI 成功](https://github.com/birdshao/xiaozhi-pet/actions/runs/40208267882)。后续任务：为 `kid_app` ESP-IDF 组件加入音频策略文件，创建 `AudioPort` mock/适配器，并做真实 I2S/语音抢占测试。
