# 2026-10-25：微雪 3.97 英寸板纯官方小智基线烧录与真机启动

**状态：已在真实 ESP32-S3 上写入并确认启动**。这是供后续宠物开发作参考的干净固件基线。

## 源码与产物

- 原版开源项目：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，tag `v2.5.2`，锁定 Git Commit `49b4380b3faa0ceadd28a3e78aaf0f311c46c57e`。
- ESP-IDF：6.1，板型：`waveshare/esp32-s3-epaper-3.97`。
- 编译：**不对原版上游源码做任何补丁，不添加本项目 PetDisplay/PetEngine**。
- [GitHub 纯官方固件发布](https://github.com/birdshao/xiaozhi-pet/releases/tag/official-waveshare397-v2.5.2-20261025)。
- App-only 文件：`xiaozhi-official-v2.5.2-waveshare-397-app.bin`，2,848,304 B，SHA256 `968cf86c71e13da69d157ec8fa12de3f65061c9004a7496e9373e8571893b3ac`。
- 官方默认资源：`xiaozhi-official-v2.5.2-assets-NOT_FOR_AUTO_FLASH.bin`，2,900,513 B，SHA256 `7476fd375dec28785a076143fe359fb5ea8fad3c6e420b8dabb8f0c158a2fc3e`。
- `sdkconfig` 中 `CONFIG_FLASH_DEFAULT_ASSETS=y`，所以这次除了应用镜像，也在核验后更新了匹配的官方资源前缀，避免单独替换 app 造成字体/语音模型资源不匹配。

## 实际烧录证据

通过组织级 Windows Self-hosted Runner `DESKTOP-QMOCC9V`（ESP32-S3 / COM10）。

- [一次性真机工作流成功运行 #40203387106](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40203387106)。
- [执行代码审计 PR #21](https://github.com/smoothRobot/orpa-agent-runner/pull/21)：**已关闭，未合并**，避免误触发重复物理写入；Runner 本地设置了一次性操作标记。
- 先核验私密本地 16MB 备份 SHA、旧 OTA1 启动记录、分区表、OTA1 app 头、之前 OTA0 宠物镜像 SHA 及 USB 身份、安全标志。
- 将纯官方 app-only 写入 `ota_0`（0x20000），读回完整 app 并核验 SHA-256。
- 将官方默认资源包写入 `assets` 分区起始位置（0x800000），读回已写入的 2,900,513 B 并核验 SHA-256。没有擦除剩余资源分区末尾。
- 更新 `otadata` 第一条 4096B 选择记录，选择 OTA0，并读回确认；OTA1 选择记录未改动。
- 启动串口摘要收集 124 行，识别到应用版本 2.5.2，未检测到匹配的 panic/watchdog 标记。
- 成功标记：
  ```text
  OFFICIAL_DOWNLOAD_VERIFIED: app=2848304 assets=2900513 bytes
  PREFLIGHT_ALL_PASS: backed up OTA1, OTA selectors, Pet image, partitions and assets
  APP_OFFICIAL_READBACK_OK
  OFFICIAL_ASSETS_READBACK_OK
  OTA0_SELECTED_AND_VERIFIED
  BOOT_SUMMARY: lines=124 version=True panic=False watchdog=False
  OFFICIAL_BOOT_CONFIRMED: pure XiaoZhi v2.5.2 OTA0 active; OTA1 old app preserved
  ```

## 现在 Flash 上是什么？

| 区域 | 当前内容 |
|---|---|
| OTA0 | **纯官方 XiaoZhi 2.5.2**，已选中并经串口启动检测 |
| OTA1 | 原有用户设备上的 2.2.38 应用（**不能称为官方原版**），未改写 |
| Assets | 已用纯官方 v2.5.2 的默认资源更新前 2,900,513 B，读回校验通过 |
| NVS | 本次烧录程序**没有直接写入**，但新 app 启动可能根据自身逻辑修改配置 |
| Bootloader、分区表 | 未改动，保留设备原有设置 |
| 完整原设备备份 | Windows Runner 本地 16MB 敏感备份，**未上传到 GitHub** |

**重要准确性限制**：这证明设备已加载纯官方上游的应用固件及其匹配资源，但不是“整片 Flash 全部恢复到原厂出厂映像”。因为引导程序、分区表、NVS 没有覆盖，assets 尾部也未清空。

此外 OTA1 的旧应用虽仍在，但如今共享 assets 是新版官方资源；**不能保证回退 OTA1 后语音/图形资源立刻兼容**。恢复原 OTA1 使用前，可能还需要从本地完整 Flash 备份恢复旧 assets。禁止直接把 16MB 备份上传公开仓库或 Artifact。

## 下一步 P0/P1 真机验收

- [ ] 确认墨水屏原版首页正确显示（方向、刷新、中文字体）。
- [ ] 确认 BOOT / PWR 行为未异常，麦克风、扬声器、Wi-Fi 与唤醒/聊天正常。
- [ ] 记录 ESP32 上运行时 Heap/PSRAM、高频刷新后的残影和驱动 BUSY 用时。
- [ ] 将以原版为基准重新集成 PetDisplay；基于 `kid_app` 业务组件继续开发 P2 事件队列与 NVS 存档。
- [ ] 对后续每一个宠物版本采用独立 app-only 包与显式版本兼容检查，不把编译出的整片 merged Flash 镜像直接刷入现有设备。

后续已完成的业务代码：[P2 PetController](../P2_PET_ENGINE.md)。
