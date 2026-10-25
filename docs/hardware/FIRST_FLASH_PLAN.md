# P1 首次真机烧录与恢复门禁

状态：**准备中；尚未执行新固件写入**（2026-10-25）  
目标：让 Waveshare ESP32-S3-ePaper-3.97 显示宠物表情，但保护现有固件、NVS 与 assets。

## 1. 已确认条件

| 项目 | 实测/构建结果 | 证据 |
|---|---|---|
| COM10 / Espressif 303A:1001 | Windows Runner 识别成功 | [Runner 探测](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40190617130) |
| 芯片 | ESP32-S3 rev 0.2，8 MiB PSRAM / 16 MiB Flash | 同上 |
| OTA app 槽 | ota_0/ota_1 均为 4,128,768 B | [分区只读检测](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40190783128) |
| 原有整片备份 | 本地 16,777,216 B，SHA256 验证通过 | [备份](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40191402358) |
| 独立校验 | 原备份 SHA 匹配，两 OTA app 镜像头存在 | [备份二次验证](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40192146199) |
| 宠物 app 编译 | 2,851,232 B；SHA256 `034e4640bffbf184094ece9dfaefd021d1011e50ccd9076a43291f71eff69e89` | [CI](https://github.com/birdshao/xiaozhi-pet/actions/runs/40191850961) |

### 整片备份保存与保护

- 保存于 Windows Runner 当前用户 `%LOCALAPPDATA%\\XiaozhiPet\\FlashBackups`，禁止通过 GitHub Artifact/仓库/聊天上传原始镜像。
- 备份名：`esp32s3-com10-20261025T120252Z-full16mb.bin`。
- 预期 SHA256：`56FD3AA2E5982D88880D42826D033E9F39ECB206A95A312F3C3CD1DD1028EDCC`。
- 此文件可能含家用 Wi-Fi 配置、Token 或私密业务信息，日志只能出现大小、哈希及是否通过，不能出现内容。
- 保存完毕仅代表能读取**备份文件**，还没有实际证明可以将其完整写回。

## 2. 尚未解决、不能跳过的关卡

1. **确认活动 OTA 槽**：从本地备份检查 `otadata` 两份条目的 CRC、序号和 `ota_state`，并对照当前 ESP-IDF 6.1 Bootloader 算法。原始值显示 `0x21/0x22`、状态 `0x02`，但未检查 CRC，不能仅凭“较大的序号”立即决定烧录。
2. **芯片安全配置**：只读检查 Flash 加密、Secure Boot 等状态。若启用安全功能，拒绝未经匹配签名的测试 app。
3. **区分 app-only 与 merged 镜像**：必须使用 `build/xiaozhi.bin` 这一应用镜像（记录精确 SHA256），不可直接使用包含引导程序/分区表等内容的 `merged-binary.bin` 做整片覆盖。
4. **保护 NVS 和 assets**：不擦除 `0x9000` NVS、`0x800000` assets；现有 `ota_1`/ `ota_0` 至少保留一份经过验证的可启动旧 app。实际写入目标由上一步 CRC 校验确定。
5. **升级与回滚方式**：优先通过 ESP-IDF 的 OTA 接口完成写入和切换，或者使用经过校验的分区写入与官方 OTA 工具操作，**不要手工猜测写入 otadata 的字节**。
6. **镜像和真实目标核验**：需要二次验证真正下载到 Windows Runner 的应用镜像 SHA、目标芯片、Flash 容量、软件版本和写入分区偏移；有任一不匹配就拒绝执行。
7. **执行策略**：烧录必须是独立的受控手动流程，禁止对 PR push 自动触发真实 Flash 写入、OTA 改写或整片擦除；备份保留不可覆盖。
8. **真实恢复演练**：未证明可恢复前，任何新固件启动失败都应停止进一步实验，按经过核验的回滚程序处理。

## 3. 分区与写入边界

```text
0x000000 ─────────────────────────────────
            bootloader / partition / NVS（不得在测试中覆盖）
0x020000 ── ota_0： 0x3F0000 bytes ────────
0x410000 ── ota_1： 0x3F0000 bytes ────────
0x800000 ── assets： 0x800000 bytes ───────
0x1000000 ────────────────────────────────
```

该示意省略小型分区的具体偏移，详细布局见 [BOARD_BASELINE](BOARD_BASELINE.md)。备用槽位置尚未正式判定。

## 4. 首次真机验收顺序

1. 烧录前记录当前 app 版本、设备启动串口摘要与 NVS/资源存在情况，过滤所有凭据。
2. 确认不修改当前正常启动分区后，烧录**仅 app**到已核验的非活动分区；写后校验镜像一致性。
3. 以受控 OTA 机制选择新 app 并重启，启动失败能退回原 app。
4. 首启串口检查 WDT、PSRAM、LVGL、EPD SPI 是否正常。
5. 空闲状态长按 BOOT 进入宠物预览，短按切换表情，再长按返回原版小智 UI。
6. 拍照并记录实际刷屏耗时/残影，检查是否阻塞聊天和扬声器。
7. 在失败时停止后续写入，按已准备的恢复流程回到备份的原有固件版本。

## 5. 当前阶段结论

- 已有完整的可执行源代码与 CI 编译产物。
- 真正设备的静态硬件信息、Flash 分区和备份已核实。
- **但 OTA CRC、安全配置、活动槽与回滚尚未验证，暂不执行写入。**

后续不能用本地备份“存在”来取代这些安全门禁。