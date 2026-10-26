# BIRDSHAO Runner 真机联调入口（2026-10-25）

**用户确认新的开发板连接位置为名为 BIRDSHAO 的 Windows Self-hosted GitHub Runner。** 先前的 DESKTOP-QMOCC9V / orpa-otb-gs01-desktop 与 COM10 不再作为硬件真值，也不能据此继续烧录。

## 1. Runner 只读识别

组织仓库 `smoothRobot/orpa-agent-runner` 已合并 [PR #23](https://github.com/smoothRobot/orpa-agent-runner/pull/23)，新增 [只读 BIRDSHAO 串口识别工作流](https://github.com/smoothRobot/orpa-agent-runner/actions/workflows/xiaozhi-birdshao-readonly.yml)。

- 需要有权限的 GitHub 账号进入工作流，选择分支 `main`，**手动 Run workflow**。
- 首先必须验证环境变量 `RUNNER_NAME == BIRDSHAO`。GitHub 的 Runner **名称不是自动生成的调度标签**，因此先使用 `[self-hosted, Windows, X64]` 安排；若被旧 Runner 抢占，任务会在第一步拒绝，不执行 USB 查询。
- 在正确的 BIRDSHAO 上只打印串口 `COMx` 和已识别 USB 家族，不输出 USB 实际序列号、设备 MAC 或敏感备份路径；**不执行 esptool、不触碰 Flash**。
- 仅当工作流报告 `RUNNER_TARGET_OK`、`ESP32_USB_DETECTED: port=COMx`、`READ_ONLY_INVENTORY_COMPLETE` 后，才将新的实际端口绑定到后续真机测试工作流。若未显示上述三项，先排查 Runner 上线/USB 数据线/驱动。
- 如果需要稳定分配给 BIRDSHAO，可在 GitHub 的 Runner 管理 UI 给该设备增加独立自定义标签，再将任务 `runs-on` 改为该标签，而不是猜测 Runner 名称可直接匹配。

## 2. 当前真实 Flash 安全边界

- [之前纯官方小智 v2.5.2 真机安装成功](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40203387106)；真实官方 app 与配套官方 assets 已安装在 OTA0，并通过启动串口摘要验证。
- 原旧版本 OTA1 是历史非官方小智程序。**未来独立宠物程序应优先通过 OTA1 测试，并保留可启动的官方 OTA0**，不得将整个合并固件覆盖到 Flash 起始地址。
- 官方 resources / NVS 与 OTA 应用槽可能为共享，OTA1 启动仍可能更改全局 NVS。部署前需要在新 Runner 重新验证原始完整备份，确认备份位于正确计算机，必要时在新 Runner 上新建并校验受限备份；不能假设旧 COM10 机器的私密备份已复制到 BIRDSHAO。
- 读取分区表、当前 otadata 与官方 OTA0 的 readback 应先通过只读工作流完成，然后再批准 app-only OTA1 写入。绝不能根据旧记录硬编码最新的活动槽、串口或资源大小。

## 3. 代码已完成、真机仍未完成的板卡入口

[PR #21](https://github.com/birdshao/xiaozhi-pet/pull/21) 为*实验性宠物固件*的微雪板型添加：

- BOOT 长按（小智 Idle）进入/退出 Pet 模式；普通模式点击仍控制小智语音，PWR 不改变。
- Pet 模式：单击互动/故事翻页；双击打开绘本；三击喂食。
- 用户开始使用宠物时，才创建 FreeRTOS Pet 服务、显示邮箱、显示 worker；宠物 NVS 加载不合法则拒绝启用，不执行擦除。
- 双槽存档与事件 ID 防重，再合并到一个场景邮箱，由独立 5s 全刷节流的显示任务渲染。
- 退出时保持互斥，防止耗时的 ePaper 操作在 LVGL 切回后再次覆写。
- 局部刷新和表情延迟尚未实体测量；本轮固件仍为 CI 编译产物，不等于已刷到 BIRDSHAO 板子。

## 4. 物理验收步骤

1. BIRDSHAO 在线 + 唯一 ESP32-S3 串口识别；
2. 新 Runner 受限本地 Flash 完整备份与 SHA 校验，核对官方 OTA0 及当前启动槽；
3. 编译实验性 app-only 镜像 SHA、OTA1 分区边界、board ID；
4. 必须另行创建具备精确授权、严格设备状态与 readback 校验的 OTA1 手动写入脚本；未经确认不自动执行；
5. 把实体设备置于测试现场：长按/单击/双击/三击 BOOT；记录 PET_RUNTIME_ON/OFF、PET_EVENT_COMMITTED、显示残影、BUSY、语音是否保留、掉电 NVS；
6. 出错时首先停留在可读状态，采用已验证的手动启动槽恢复；不要重用旧 COM10 恢复脚本。

此文档和目前代码**不触发任何真机烧录或板卡复位**。
