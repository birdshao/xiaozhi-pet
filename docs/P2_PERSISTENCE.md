# P2-05：宠物离线双槽持久化（v1）

**实现时间**：2026-10-25。当前代码已接入可编译的 `kid_app` 组件，但**尚未在真机 NVS 上做断电/寿命验证**；官方 OTA0 固件仍不含这些改动。

## 目标与边界

设备离线时，宠物成长值、当前阅读页、安静模式和最近 128 个已完成事件 ID 需要在重启后恢复。音频播放、监听及语音会话状态不能自动恢复。不能让网络重传或意外重启造成重复奖励，也不能因写入中途断电毁掉最后一份有效进度。

关键模块：

- `PetController::ExportCheckpoint / RestoreCheckpoint`：一次导出完整进度与去重窗口，验证失败不修改当前内存；重启后的 `Listening/Speaking` 强制回到 `Idle`。
- `PetCheckpointCodec`：定长 **1064B** 的小端序 v1 格式，头包含 magic `KPET`、版本、长度、64-bit generation；末尾 CRC32。编码显式地逐字段写入，不用 `memcpy(struct)`，避免 C++ 对齐、编译器和 MCU 端序差异。
- `PetPersistence`：轮换写 `pet_a` 与 `pet_b` 两份 blob，仅新槽提交成功后更新当前代号；启动时选择校验通过的最大 generation。损坏的较新槽不会覆盖较旧但有效的快照。
- `NvsPetBlobStore`：ESP-IDF `nvs_open/nvs_get_blob/nvs_set_blob/nvs_commit` 适配器；独立 NVS 命名空间 `kid_pet`，不接触小智已有业务 Key，也不会调用 `nvs_flash_erase`。
- `SaveIfDue`：非关键写入由上层按单调毫秒时间节流；**喂食奖励、家长安静策略、重要成长进度必须立即持久提交后再 ACK**。内存事件本身不是持久成功确认。
- `PetEventExecutor::ApplyDurably`：单线程事务入口，先暂存内存状态，在成功提交双槽后才返回 `Committed` 与 UI/音效效果；提交失败则恢复内存状态及事件去重窗口，返回 `StorageError`，由调用者决定重试。不要在失败时提前播放奖励音效。

## 二进制协议

| Offset | Bytes | 含义 |
|---|---:|---|
| 0 | 4 | magic `KPET` |
| 4 | 2 | format version=1 |
| 6 | 2 | total length=1064 |
| 8 | 8 | generation（初始=1，双槽轮换递增） |
| 16 | 4 | PetSnapshot schema=1 |
| 20 | 1 | mode |
| 21 | 1 | mood |
| 22 | 1 | quiet (0/1) |
| 23 | 1 | reserved=0 |
| 24 | 4 | interaction_count |
| 28 | 2 | active_page |
| 30 | 2 | event ID 数量 |
| 32 | 2 | 下次替换位置 |
| 34 | 2 | reserved=0 |
| 36 | 1024 | 最近 128 个 `uint64_t event_id` |
| 1060 | 4 | CRC32 of bytes 0..1059 |

CRC32 只防非恶意数据损坏，**不是数字签名**。schema 或检查不通过时读取失败，不静默忽略未来格式。

## 故障与事务保证

1. 首次写 A，第二次写 B，第三次回 A。任意提交失败时旧槽不替换；下一次启动选择有效的最大 generation。
2. 若旧槽有效、新槽 CRC 错误，回退旧槽，**不会获得最后未持久化的那次奖励**；不可对家长承诺每一次尚未 ACK 的动作绝不丢失。
3. 最近 128 个 `event_id` 与状态同事务持久化，重启后重复事件不会再次增加成长计数。跨 128 个新事件后的旧 ID 不再被窗口识别，因此 P3 服务端还需幂等事件日志，不把这个窗口误当永久去重。
4. `Listening/Speaking` 不会在重启后继续运行；通过新事件才能开启麦克风。
5. 未知 format version、非法 enum、重复 event ID、损坏 CRC 和无效页面会拒绝恢复，不修改当前 PetController。
6. 双槽依赖 ESP-IDF NVS 单个 Key 的 blob 写入/提交具备断电恢复能力；**模拟器验证不等于板卡掉电实测**。NVS 分区仅 16 KiB，必须节流并核算寿命、额外数据及 NVS 存储容量。
7. NVS 初始化和调度由宿主主程序负责：初次运行请先初始化 `nvs_flash`，再构造 `NvsPetBlobStore`、加载快照，然后开始接受新事件。不要在显示线程或中断里直接写 Flash。

## 测试运行

```bash
bash scripts/p2_test.sh
```

`tests/cpp/test_pet_persistence.cc` 使用内存模拟 NVS，验证编码、CRC、代号选择、较新槽损坏回退、写入故障模拟、重启后的幂等事件及语音状态归零、节流策略、128 事件环形窗口。运行 GitHub Actions [P2 deterministic pet state machine](../../.github/workflows/p2-pet-engine.yml) 与集成 ESP-IDF 编译，确保 host 和板载编译共同通过。

## 后续硬件任务（未完成）

- [ ] 将 NVS 加载与提交 Hook 接入正式 `PetController` 调度任务，确保关键事件的 ACK 不早于持久写入。
- [ ] 受控测试前备份用户原 NVS；确认命名空间 `kid_pet` 不与旧官方/设备数据冲突。
- [ ] 在**非官方 OTA0 的测试部署**中执行 100 次人工重启/掉电模拟，验证双槽回退、NVS 余量与 PSRAM。
- [ ] 实测写入耗时与磨损风险，确定事件 ACK/Flash 队列及写入节流的具体策略。
- [ ] 升级旧格式的迁移策略及服务端长期幂等对账（P3/P5）。
