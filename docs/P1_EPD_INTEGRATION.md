# P1：接入实际微雪驱动的宠物预览（编译与真机验证计划）

状态：实验阶段。基于固定的 [小智 ESP32 v2.5.2](https://github.com/78/xiaozhi-esp32/releases/tag/v2.5.2)。本分支的云端 CI 构建 **不进行烧录**。

## 1. 最小改造

`scripts/p1_integrate.py` 在经过 Commit 核验的、可丢弃的小智源码 checkout 中实施四处受控修改：

1. 复制 `firmware/components/kid_app` 到上游 `components/kid_app`，引入纯 C++ 宠物黑白画面渲染。
2. `main/CMakeLists.txt` 声明 `main` 对 `kid_app` 的依赖。
3. `CustomEpdDisplay` 新增 `ShowPetPreview / ExitPetPreview`。只读 SceneModel 渲染到 PSRAM 中的 48KiB 1bpp 帧，再把帧复制给微雪物理帧缓冲，使用现有墨水屏驱动刷新；在预览期间暂停物理 LVGL flush，但逻辑控件仍然运行。
4. 在这块板卡的 BOOT 按键长按入口加入一个临时演示模式：待机时长按进入或退出宠物预览；预览中短按依次切换表情。未进入预览时，保留现有 BOOT 点击的语音功能，PWR 关机按键不变。

> 这不是生产级 RefreshScheduler：初版使用较重的完整帧刷，避免一开始同时改写局刷寄存器。屏幕刷新任务独立于按钮回调和小智主事件循环；有“忙碌时拒绝重复刷新”的保护。P1 仍需真实硬件测试慢刷新、鬼影、音频调度和复原原版 UI 的行为。

## 2. 可复现编译

在 Linux / ESP-IDF 6.1 环境：

```bash
bash scripts/p0_checkout.sh
python3 scripts/p1_integrate.py --source .upstream/xiaozhi-esp32
source "$IDF_PATH/export.sh"
cd .upstream/xiaozhi-esp32
python scripts/build.py waveshare/esp32-s3-epaper-3.97 --name esp32-s3-epaper-3.97
```

GitHub CI：`.github/workflows/p1-integrated-firmware.yml`，只编译不烧录，同时检查 `build/xiaozhi.bin` 是否不超过实测 OTA 分区大小 **4032 KiB**。

## 3. 真机验证（后续操作）

- [x] BIRDSHAO 原始 Flash 16MiB 完整私有备份存在，SHA256 已校验，未上传 GitHub。
- [x] 已保存原完整 Flash 和旧 OTA1 镜像，并提供手动 OTA1 还原脚本；首次切换启动槽的独立 OTADATA 恢复工具尚未验收。
- [x] 已校验正式 app-only artifact 的 SHA256、board 类型、原始分区表和 16MiB Flash；在实体备用 OTA1 完成烧录与两次独立读回校验。
- [ ] 初次上电观察串口异常与看门狗。
- [ ] 在空闲状态长按 BOOT 进入宠物预览。
- [ ] 短按 BOOT 切换表情：开心、好奇、困倦、普通。
- [ ] 长按 BOOT 退出，LVGL 原版状态恢复。
- [ ] 进入/退出 20 次检查屏幕残影/卡顿、heap 使用、音频是否异常。
- [ ] 记录刷屏耗时、第一次启动耗时、设备复位次数，拍照留证。
- [ ] 如出现无法返回正常聊天，优先用本地备份恢复。

**硬件注意**：原有设备 OTA0/OTA1、assets 和 NVS 不应为了测试被全部擦掉。预览固件的写入策略须与真实运行分区匹配；不能仅根据整个 `merged-binary.bin` 文件名推测正确烧录地址。

## 4. 与后续代码维护的关系

此实验性适配只针对微雪板卡的 `CustomEpdDisplay` 与按键回调，Pet Scene 数据模型不依赖小智内部类。后续可把这一临时 Demo 变成正常的 `PetController` / `RefreshScheduler`，并将 C++ patch 缩减成稳定的适配 Hook，方便合并上游新版本。

相关： [P1 原型](P1_SCENE_PROTOTYPE.md)、[系统架构](ARCHITECTURE.md)、[P0 真机基线](hardware/BOARD_BASELINE.md)。

## 5. CI 集成编译结果（2026-10-25）

[PR #9](https://github.com/birdshao/xiaozhi-pet/pull/9) 已合并到 `main`。

[GitHub Actions 完整构建成功](https://github.com/birdshao/xiaozhi-pet/actions/runs/40191850961)：

- 工具链：`espressif/idf:v6.1`
- 原始小智固件：`v2.5.2` / Git SHA `49b4380b3faa0ceadd28a3e78aaf0f311c46c57e`
- 集成：`kid_app` 组件 + 微雪 `CustomEpdDisplay` + BOOT 演示事件
- 应用镜像 `build/xiaozhi.bin`：**2,851,232 B**
- 实测 `ota_0/ota_1` 单槽：**4,128,768 B**
- 剩余容量：**1,277,536 B**（未扣除将来其他功能）
- 应用固件 SHA-256：`034e4640bffbf184094ece9dfaefd021d1011e50ccd9076a43291f71eff69e89`
- CI Artifact 名：`pet-preview-epd-buildonly`（`merged-binary.bin`，仅限测试，7 天保留）。
- **CI 仅证明代码能完整编译，尚未证明硬件可运行。**

切忌直接把 `merged-binary.bin` 整片覆盖到现有设备：该文件可能包含 bootloader、分区表或默认资源，而现有分区内已存储用户数据和两套可启动固件。第一次写入必须遵守 [首次受控烧录计划](hardware/FIRST_FLASH_PLAN.md)。

## 6. 实际 OTA1 备用分区烧录（2026-10-27 UTC）

之前的 CI-only P1 适配已向真实设备推进了一步：`main` 上的 Pet app-only 固件 2,857,408 B，SHA256 `b643437eda9ccff1f7d21bd8822a31253906613286b892b1f6b5d8caa4bb1dda`，已写入 BIRDSHAO 的 `ota_1` 分区（地址 `0x410000`）。[真机写入与读回成功的 Runner 任务](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40450062505) 及后续独立只读审计均验证 SHA256，官方 OTA0 保持完整、OTADATA 未改变、本地事务日志为 `stage_verified`。

**注意：写入成功不等于已经运行 Pet 应用。** 官方 OTA0 仍被选中，下面的实体按键/墨水屏/音频/恢复验证保持未完成。只有在确认安全的启动槽回退方案并获得另一次许可后，才能进行短时首次启动实验。详见 [OTA1 实际烧录与首次启动门禁](hardware/OTA1_STAGED_BOOT_GATE.md)。
