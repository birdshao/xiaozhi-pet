# P2 设备端事件任务与 NVS 事务联动

状态：C++ 实现与 ESP-IDF 组件集成；**尚未挂载到官方 OTA0 的按钮/语音入口**，也没有在实体设备运行。本功能由独立 Pet 固件构建/真机试验另行启用。

## 数据流

```text
按钮任务 / IMU / 语音事件 / 其它生产者
  ├─ 普通任务 Post(PetEvent)      → FreeRTOS xQueueSend(timeout=0)
  └─ 中断 PostFromISR(PetEvent)   → FreeRTOS xQueueSendFromISR()
                                          │
                                 bounded inbox(24)
                                          │
                                 kid_pet FreeRTOS task
                                          │
                           PetEventRuntime<1> [retry pending]
                                          │
                           PetEventExecutor.ApplyDurably()
                                          │
                      PetController → PetPersistence → NvsPetBlobStore
                                          │
                             commit success / duplicate / rejected
                                          │
                            PetCommitCallback / outgoing queue
                                          │
                          UI刷新队列 / AudioCoordinator / ACK
```

**核心顺序：**状态更改和本次事件 ID 先一起落盘，成功后才能 ACK 或播放奖励音效。失败时恢复内存状态，保持同一个事件在队头，等待后续重试。排队已满则拒绝新事件，不假装接受它。

## 初始化接口（宿主须显式调用）

```cpp
#include "kid_app/freertos_pet_service.h"

static kid_app::FreertosPetService pet_service; // 生命周期不短于任务

// 由宿主先完成 nvs_flash_init()；如其失败，禁止启动服务。
// 控制台/屏幕/音频是各自的独立线程与队列。
void on_pet_committed(const kid_app::PetEvent& event,
    kid_app::PumpResult result, const kid_app::PetEffects& effects,
    const kid_app::PetSnapshot& snapshot, void* context) {
    // 这里只做最小非阻塞派发，不直接调用 EPD BUSY 等待、NVS 或语音 API。
    // 只有 result == PumpResult::Committed 才能发正向奖励或状态变更 ACK。
}

bool begin_pet_task() {
    return pet_service.Start(on_pet_committed, nullptr);
}

bool enqueue_button_event(kid_app::PetAction action) {
    return pet_service.PostLocal(action); // persistent ID allocated in worker
}

bool enqueue_network_event(uint64_t stable_network_id, kid_app::PetAction action) {
    return pet_service.Post({stable_network_id, action});
}
```

- `Start` 只调用一次；若已有 NVS 两份都不可读、损坏或 I/O 错误，则拒绝启动，不自动清空/格式化。首次没有存档时可以从默认状态启动。
- 当前 FreeRTOS inbox 容量 24，`PetEventRuntime<1>` 只有一个待处理事件。NVS 失败将使整个事件流按序阻塞；依靠有界容量向输入层反馈压力。
- `Post` 为普通任务零等待发送；`PostFromISR` 使用 FreeRTOS ISR 安全 API，参数是 `BaseType_t* higher_priority_woken`，由调用方决定是否在中断退出时 yield。
- 不能从 ISR 直接更新 PetController 或写 NVS。
- `PetCommitCallback` 在工作线程上运行，不能阻塞 EPD 全刷、不能在回调内部销毁 `FreertosPetService`，应将轻量消息复制到专门的显示/音频任务。
- `FreertosPetService` 设计为设备整个运行生命周期存在，尚未实现动态停止、动态卸载；不要作为栈上临时对象创建。
- 提交成功≠屏幕已经刷新成功；显示与语音的后续 ACK 还需要独立状态和重试规则。

## Event ID 的硬性要求

**普通本地输入**请使用 `PostLocal(action)`：FreeRTOS 消息先用专用内部哨兵，进入单一工作线程后按持久快照的 `generation+1` 分配 ID，设置最高位 1。完成事务后 generation 与最近 128 个 ID 一起提交，所以重启不会直接复用此前确认过的本地 ID。

**外部/网络输入**使用 `Post(PetEvent)` 或 `PostFromISR(PetEvent)`，必须提供非 0、最高位为 0 的稳定 ID（`IsExternalEventId`），且**同一动作重传必须保留同一个 ID**。高位约定避免与设备本地事件冲突。事件已进入队列后的重试由工作任务负责，不重新编号。

如果 NVS 回退到更老代号，那么从未成功持久确认的后续本地动作可能需要人工对账；该机制不是全局无限去重。现有设备仅持久保存最近 128 条已提交 ID，长期跨设备幂等还依赖 P3 服务端事件日志。

## 运行验证

- `tests/cpp/test_pet_event_runtime.cc`：验证有界队列、回绕、存档失败队头保持、成功后 ACK、重复事件无奖励。
- `tests/cpp/test_pet_local_event_id.cc`：验证跨重启事件 ID 随已提交 generation 增长、内外 ID 不冲突、旧本地事件不会重复发奖。
- `tests/cpp/test_pet_event_executor.cc`：验证事务提交失败能回滚状态与事件去重。
- `bash scripts/p2_test.sh`：无需 ESP-IDF 的主机编译/运行。
- `.github/workflows/p1-integrated-firmware.yml`：ESP-IDF 6.1 编译 `freertos_pet_service.cc` 与完整微雪板型，产物不上板。
- **未完成：**真实板载输入接线、物理 NVS 掉电测试、FreeRTOS 堆栈/队列余量、系统级 UI/音频回调和安全事件 ID 生成器。以上未完成前，不把 P2 判为真机通过。

参阅 [P2 存档格式](P2_PERSISTENCE.md) 和 [P1 墨水屏显示](P1_EPD_INTEGRATION.md)。
