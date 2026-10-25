# P1 在 Windows GitHub Runner 上进行首次安全写入

状态：**固件包已交付，写入等待手动触发；尚未实际修改设备**（2026-10-25）。

## 已通过的自动检查

- [应用镜像预发布](https://github.com/birdshao/xiaozhi-pet/releases/tag/p1-device-preview-20261025)：
  - `xiaozhi-pet-esp32s3-app.bin`，**2,851,232 字节**；
  - app-only SHA256：`1ed92d996f4bb37f917ac9c31eecc7e4c9923fccc2d504711dc02a9ee204e69f`；
  - 构建项目提交：`9e74c9f708e532e7ce27c06ef2f60e96aa487286`；
  - `manifest.json` 记录板型、上游版本、app 长度、SHA256 和打包边界。
- [原应用构建通过](https://github.com/birdshao/xiaozhi-pet/actions/runs/40193480935)，只发布 app-only，不发布用于直接刷写整片 Flash 的合并镜像。
- [Windows Runner 只读预检通过](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40194292691)：
  - ESP32-S3，COM10，USB `303A:1001`；
  - 原厂整片 16MiB 备份大小及 SHA256 均通过；
  - 备份 `ota_0` app 版本 `2.2.37`，`ota_1` app 版本 `2.2.38`；
  - 实时读取 `otadata` 两条有效 CRC，序列 `0x21/0x22` 与原备份一致；
  - 按当前 OTA 选择规则，当前选中的启动目标为 **ota_1**，但尚无独立启动日志证明未发生 fallback；
  - 下载的测试应用 app 描述为小智 **2.5.2**，哈希和大小与 manifest 相符；
  - 允许候选备用写入槽为 **ota_0**，偏移 **0x20000**。
- 芯片 Secure Boot、Flash Encryption 检查均为 Disabled；未进行写入。

## 手动测试工作流（已部署）

[打开组织 Runner 的“Xiaozhi P1 stage inactive OTA0 only (MANUAL)”工作流](https://github.com/smoothRobot/orpa-agent-runner/actions/workflows/xiaozhi-manual-stage-ota0.yml)

此工作流由 [PR #16](https://github.com/smoothRobot/orpa-agent-runner/pull/16) 提交并合并到 `smoothRobot/orpa-agent-runner` 主分支，其 Python 和 GitHub Action 入口经过静态检查（[CI](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40194729837)）。

启动方式：

1. 使用具有相应权限的 `birdshao` GitHub 账号打开链接；
2. 点 **Run workflow**，分支选择 `main`；
3. 在 `confirm` 下拉选择精确值 **`STAGE_OTA0_KEEP_OTA1`**（默认 `NO_WRITE`）；
4. 点 **Run workflow**；随后在运行日志中确认最终出现 `STAGE_SUCCESS`，否则不要继续切换启动槽。

**影响范围：这会覆盖原 OTA0 中的小智 2.2.37 应用**。执行前请确保目前愿意把 OTA0 用作试验槽。当前 OTA1 的小智 2.2.38、NVS 和 assets 按工作流设计保留。

### 工作流强制做哪些检查？

- 只允许指定 GitHub 操作账号，从主分支手动启动；
- 每次写入前重新核验本机私密完整备份、现有分区表、当前 `otadata` CRC 与原备份是否一致；
- 校验 USB VID/PID、芯片安全标志、固定 Release app SHA256、镜像源代码 Commit；
- **只调用一次 `esptool write_flash 0x20000`**，将已批准的 app-only 镜像写到 OTA0；
- 写后 `read_flash` 同长度读取并重新计算 SHA256，要求与发布镜像完全一致；
- 再次读取 `otadata` 和原 OTA1 镜像头，比对备份数据，确认活动启动目标未被改动；
- 不上传原 16MiB Flash 备份，不碰 NVS、assets，不切换 OTA 启动目标。
  
如果任何校验失败，将停止并输出失败原因；如果硬件写入中途失败，原先启动的 OTA1 仍应被保留，但不能未经检查直接切换到未验证的 OTA0。

## 下一阶段：切换到新固件和宠物界面验收

**手动写入 OTA0 不是启动新版固件**。只有 `STAGE_SUCCESS` 且回读一致后，再设计/执行使用 ESP-IDF 官方 `otatool.py switch_ota_partition --slot 0` 的受控切换工作流。切换之后应从串口、设备状态和实体墨水屏验证 v2.5.2 首启与宠物表情；若异常，应优先恢复原 OTA1 启动，并避免重写 NVS/assets。

由于当前的 2.2.38 → 2.5.2 跨版本变化，启动成功后仍需检查 NVS schema、音频配置和 assets 兼容性，不能把编译成功当成系统功能完全兼容。

**当前仍未完成的事项**：真实写入、读回校验、运行时日志、EPD 局刷与残影、退出返回正常语音模式、回滚实机验证。
