# P4：Waveshare BOOT 按键的离线绘本导航闭环

这项改动不是 OTA，也不是新增一个孤立的服务端原型。它改进了板卡真正使用的 `scripts/p1_integrate.py` overlay，把物理 BOOT 按键事件映射给已经存在的 `FreertosPetService`，由原有 NVS 单任务持久化提交后再刷新墨水屏场景。

## 板卡输入规则

Pet 模式关闭时，原先的单击小智语音逻辑保持不变；长按依旧切换 Pet 模式。

Pet 模式下：

| 操作 | 宠物首页 | 本地 Reader 占位页 |
|---|---|---|
| 单击 | 互动 / 打招呼 `Pickup` | 下一页 `NextPage` |
| 双击 | 进入本地阅读页 `StartStory` | **退出阅读页** `StopStory` |
| 三击 | 喂食 `Feed` | 不处理，防止翻页时误喂食 |
| 长按 | 退出 Pet 模式 | 退出 Pet 模式 |

安静模式及录音/播放状态下，宠物输入路由不会额外触发互动动作。任何行为由 `PetInputRouter::Decide` 返回确定性决定，再由现有 `PetService::PostLocal` 排队；回调不能绕过本地存档直接修改表情和成长计数。

## 实现边界

- 已有的 SceneComposer 能在 800×480 黑白屏上区分 Home 与 Reader，显示猫脸与页码指示点。现在双击可以离开 Reader，不会一直停在阅读占位页。
- **这仍然是离线 Reader 占位页面**，尚未把服务端已审核中文绘本文字 / 语音音频同步到 ESP32 的屏幕与扬声器；不可说成能在真机完整讲故事。
- `PetInputRouter` 是纯 C++17、无硬件依赖的策略模块；按钮实物的多击时序与按键去抖仍需实体测量。
- 这次不切换 FreertosAtomicPetService，也不更改旧 `kid_pet` NVS 格式。

## 验证

`bash scripts/p2_test.sh` 中的 `test_pet_input_router.cc` 覆盖 Idle→Story→Page2→Idle→Feed 的真实 PetController 交互状态转移，并确认语音/安静模式不会意外处理按键。P1 integration overlay 合约测试确认 Board 使用三个统一路由动作。ESP-IDF 完整交叉编译校验新增源文件与 Waveshare 代码适配；上述自动化测试不替代硬件按键测试。
