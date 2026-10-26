# BIRDSHAO Runner 真机联调入口（2026-10-25）

**用户确认新的开发板连接位置为名为 BIRDSHAO 的 Windows Self-hosted GitHub Runner。** 先前的 DESKTOP-QMOCC9V / orpa-otb-gs01-desktop 与 COM10 不再作为硬件真值，也不能据此继续烧录。

## 1. Runner 只读识别

组织仓库 `smoothRobot/orpa-agent-runner` 已合并 [PR #23](https://github.com/smoothRobot/orpa-agent-runner/pull/23)，新增 [只读 BIRDSHAO 串口识别工作流](https://github.com/smoothRobot/orpa-agent-runner/actions/workflows/xiaozhi-birdshao-readonly.yml)。

- 需要有权限的 GitHub 账号进入工作流，选择分支 `main`，**手动 Run workflow**。
- [2026-10-25 修复 PR #24](https://github.com/smoothRobot/orpa-agent-runner/pull/24)：最初 [#40252978001](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40252978001) 误分配至旧 `DESKTOP-QMOCC9V2`，第一步阻止并未访问 USB。已改为 `runs-on: [self-hosted, Windows, X64, xiaozhi-birdshao]`。**必须由组织管理员在 GitHub > smoothRobot > Settings > Actions > Runners > BIRDSHAO 的 Labels 中为该 Runner 单独添加 `xiaozhi-birdshao`**，并确认只在这一台上分配该标签。Runner 名称不是自动标签，没有标签时作业只会排队（不是脚本错误）。
- 调度命中后仍必须验证环境变量 `RUNNER_NAME == BIRDSHAO`。如果将标签误加到其它 Runner，工作流会主动拒绝，不执行 USB 查询。
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

## 2026-10-25 实机 BIRDSHAO USB 检查已完成

[只读 USB 工作流 #40256890982](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40256890982) 实测：

- `RUNNER_NAME=BIRDSHAO`，机器也是 `BIRDSHAO`。
- Windows 串口枚举得到 COM3、COM4（其它设备）及 **COM10（Espressif USB VID:PID 303A:1001）**；仅一个 ESP32 USB 命中。
- `READ_ONLY_INVENTORY_COMPLETE`，工作流无 Flash 操作。**COM10 是这次在新 Runner 上实际确认的端口，而不是沿用旧电脑记录。**

### 新 Runner 本地 Flash 备份与 OTA 审核工作流

[PR #25](https://github.com/smoothRobot/orpa-agent-runner/pull/25) 已合并，新工作流入口：

[**XiaoZhi BIRDSHAO private Flash backup and OTA baseline (READ ONLY)**](https://github.com/smoothRobot/orpa-agent-runner/actions/workflows/xiaozhi-birdshao-baseline.yml)

手动选择 `main`，确认值为 `PRIVATE_BACKUP_READ_ONLY`。这个作业仅在独立 `xiaozhi-birdshao` Runner 标签且实际名称为 BIRDSHAO 的机器上执行。读取完整 16MiB Flash 到 **BIRDSHAO 本机** `%LOCALAPPDATA%/XiaozhiPet/FlashBackups/` 中新建的唯一文件，校验 SHA256 并输出不包含用户原始 NVS/凭证的摘要。禁止上传 Flash、自动烧录、修改 OTA。**esptool 只读也可能将芯片临时切入 ROM 下载模式，读完会复位设备。**

将只读摘要中的 `PARTITION_LAYOUT_MATCH`、`SELECTED_OTA`、`OFFICIAL_OTA0_APP_SHA_MATCH`、`OFFICIAL_SHARED_ASSETS_SHA_MATCH`、`PRIVATE_BACKUP_OK` 作为下一轮是否准备 OTA1 宠物测试的门槛。此流程**尚需用户手动运行**，不能把 CI 成功误认为已经在 BIRDSHAO 上创建了备份。

## 2026-10-25 Desktop Commander 直接控制已验证

BIRDSHAO 设备现在可通过已授权的 **Desktop Commander Remote** 直接操作，无需每次手动运行 GitHub Actions。真实远程 shell 核验：Windows 主机名 `BIRDSHAO`，Python `3.13.9`，唯一 Espressif USB `303A:1001` 端口 `COM10`。

BIRDSHAO 上 `esptool==4.8.1` 的原始完整 `read_flash 0x0 0x1000000` 发生 USB 数据帧丢失：`Corrupt data, expected 0x1000 bytes but received 0xfb5 bytes`。这属于传输通道的错误，**没有证据表明 Flash 数据自身损坏**。只读小块读取验证为 8KiB、64KiB、256KiB、512KiB 成功；1MiB 连续读取失败。经 [Runner PR #26](https://github.com/smoothRobot/orpa-agent-runner/pull/26) 完成容错改造，以默认 512KiB 的分段读取，失败时减半到 8KiB；每段必须完整返回、最终整片 16MiB SHA256 校验才认定备份可用。原始备份永不上传到 GitHub。

**注意**：Remote 临时执行环境并不是 OTA 烧录授权。尚未明确确认独立 OTA1 的镜像哈希、当前分区和可靠回退之前，任何真机软件写入继续禁止。官方 OTA0 保持保留。完整备份是否成功以最终 `PRIVATE_BACKUP_OK` 标记为准，不能凭局部读取进度认定成功。
