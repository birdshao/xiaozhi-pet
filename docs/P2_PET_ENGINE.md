# P2 独立宠物事件引擎：首个可编译实现

状态：2026-10-25；PR [#12](https://github.com/birdshao/xiaozhi-pet/pull/12) 已合并。

## 已落地的代码

- `firmware/components/kid_app/include/kid_app/pet_controller.h`
- `firmware/components/kid_app/src/pet_controller.cc`
- `tests/cpp/test_pet_controller.cc`
- `scripts/p2_test.sh`
- `.github/workflows/p2-pet-engine.yml`

### 核心设计

`PetController::Apply(PetEvent, PetEffects*)` 只处理一个明确的业务事件并输出状态和副作用提示，不直接调用小智音频、LVGL 或硬件 SPI。

已实现的状态/行为：

| 事件 | 规则与效果 |
|---|---|
| `Pickup` | 进入互动，显示开心表情 |
| `Feed` | 确定性增加一次互动次数；重复事件 ID 不重复生效 |
| `StartStory / NextPage / StopStory` | 控制故事场景与页码，防止越界 |
| `VoiceBegin / VoiceReplyBegin / VoiceEnd` | 对话录音、播报、结束的显式状态切换 |
| `QuietBegin / QuietEnd` | 在设备本地控制安静时段；拒绝安静时段的新语音互动 |

业务引擎不依赖上游 `Application`、`Display` 或音频编解码器；纯 C++17 测试已通过，也已经通过包含 `kid_app` 的整体 ESP-IDF 编译。

## 边界：不能误认为功能已全部完成

1. 去重仅保留最近 128 个事件 ID 的 **RAM 缓冲**。它不能保证跨重启、离线重传后的永久幂等；需实现 P2-05 持久化事件 ID / 序列号与原子快照。
2. 还没有事件生产者/消费者队列；实际运行仍需 `InputManager` 与事件主循环实现单任务串行。
3. 目前状态机尚未与真实 PetDisplay、BOOT/IMU 或音频播放协调器联动。
4. 仍未实现家长身份确认、云端对话审核、文件阅读及儿童使用安全验收。
5. 不使用饥饿死亡、负面情绪惩罚等促使儿童频繁使用的成长规则。

## 本机重现

```bash
bash scripts/p2_test.sh
```

后续路径：先完成官方小智固件真机基线，再实现 `PetEventQueue`、NVS 版本化存档、PetDisplay 映射和 BOOT 控制。保证儿童离线交互与自建 Python 语音服务端松耦合。
