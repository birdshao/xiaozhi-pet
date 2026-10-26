# P1/P2：电子纸慢刷新队列（v1）

**适用设备**：Waveshare ESP32-S3-ePaper-3.97，800×480 单色屏。

当前实现已经是 `kid_app` 的可编译组件；**不代表实际物理局部刷新已经实现或验收**。已在真机安装的官方 OTA0 不包含本功能，本次仅源码与 CI。

## 目标

原先的 BOOT 临时预览会为每次点击执行完整刷新。为了避免事件队列直接阻塞墨水屏的 BUSY 引脚，并减轻频繁刷新造成的残影，新增两层：

- `FreertosPetSceneMailbox`：在 **存档成功**、且 `PetEffects.repaint=true` 时，将 `PetSnapshot` 转为小尺寸 `SceneModel` 并用 FreeRTOS 长度为 1 的覆盖队列提交给显示任务。消费者尚未就绪、存档失败、重复事件、无画面变化时，不会执行物理刷新。覆盖队列只保留最新目标，因此多次翻页可以合并，不需缓存多张 48 KiB 图。
- `PetRefreshGate`：显示任务单线程的纯 C++ 调度器，默认最短 **5 秒**完成间隔。需 `Request(scene)` → `Acquire(now_ms,&scene)` → `Show(scene)` → `Complete(ok,completed_ms)`。在刷新成功前绝不更新已显示状态；失败后保留待刷新帧以重试。

## 宿主调用示例

```cpp
static kid_app::FreertosPetSceneMailbox mailbox;
static kid_app::FreertosPetService pet_service;
static kid_app::PetRefreshGate refresh(5000);

// 宿主已完成 nvs_flash_init，且不在 ISR 中调用：
bool begin_pet() {
    if (!mailbox.Initialize()) return false;
    return pet_service.Start(&kid_app::FreertosPetSceneMailbox::OnCommit, &mailbox);
}

// 以下只能在单个显示任务中执行，并且与原版 LVGL
// 锁/EPD SPI 的访问互斥。这里只是伪代码，不可直接刷写：
void display_task() {
    for (;;) {
        kid_app::SceneModel newest;
        if (mailbox.Receive(&newest, /* timeout */ pdMS_TO_TICKS(100)))
            refresh.Request(newest);
        kid_app::SceneModel frame;
        const uint64_t now_ms = /* monotonic time from esp_timer */ 0;
        if (!refresh.Acquire(now_ms, &frame)) continue;

        // 1. 使用 PSRAM 分配 MonoFrame，不要放入小栈；
        // 2. 渲染 SceneComposer::Render(frame, ...)；
        // 3. 在统一 EPD/LVGL 互斥后调用真实驱动刷新；
        // 4. 必须在物理刷新完成后报告结果和时间。
        bool physical_epd_ok = false; // TODO: board-specific glue.
        refresh.Complete(physical_epd_ok, now_ms);
    }
}
```

本示例不是可直接使用的真机入口，实际显示任务和物理驱动适配仍待完成。不能将 `Complete(true)` 提前放在 SPI 命令发出时，应等屏幕 BUSY 完全结束。

## 不变量

1. 逻辑宠物状态与画面呈现解耦：存档 ACK 不等待数秒的电子纸刷新；显示失败不会重复发奖或重放儿童语音。
2. 单帧完整刷新期间不能同时发另一帧；来自按键、语音、绘本的多次更新只保留最后一次画面。
3. 如果最后一幅画面与最新目标完全一致，不执行多余的完整刷新；书页号也是比较字段。
4. **刷新冷却时间从物理刷新完成时刻开始**（不是从按键点击时起），失败不前移计时器。
5. 仅在渲染任务中访问 `PetRefreshGate`，不能让 FreeRTOS callback 和显示驱动并发修改它；`FreertosPetSceneMailbox` 是跨线程同步边界。
6. 暂不启用物理局刷，因为尚未核实硬件支持程度、驱动 BUSY 时序和具体残影。5 秒仅是保守的工程默认值，不是已实测器件寿命或最优参数。
7. 必须给故事页快速翻页以反馈（例如音效、状态指示），不能假设电子纸能即时呈现每一页；家长静音事件也要可靠保存，画面可等到合适刷新窗口再更新。

## 测试

`tests/cpp/test_pet_refresh_gate.cc` 验证首次刷、无变化跳过、持续刷新间隔、状态合并、刷屏时来了新状态、失败立即重试、时钟回退保护。运行 `bash scripts/p2_test.sh`；完整 ESP-IDF 构建另需编译 `freertos_pet_scene_mailbox.cc`。

## 后续硬件验收

- [ ] 将邮箱队列与真正的 `CustomEpdDisplay`/LVGL 互斥 Hook 对接；
- [ ] EPD 刷新完成才 `Complete(true)`，测实际 BUSY 时长；
- [ ] 首页、绘本、Quiet 连续切换 100 次，记录丢弃数、最终显示、WDT 与残影；
- [ ] 检查语音服务期间不能与 EPD 全刷相互阻塞；
- [ ] 只在可回退的单独宠物测试分区验收，不直接覆盖已经验证的官方 OTA0。

关联 [设备事件服务](P2_DEVICE_EVENT_SERVICE.md)、[场景渲染](P1_EPD_INTEGRATION.md)。
