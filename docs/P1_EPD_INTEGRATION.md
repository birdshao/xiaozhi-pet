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

- [ ] 本地全芯片备份完成，校验 16MiB 大小与 SHA256，备份不上传 GitHub。
- [ ] 明确可用的恢复路径和原固件镜像位置。
- [ ] 从编译 artifact 验证映像 SHA256、board 类型、分区表与 Flash 容量。
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
