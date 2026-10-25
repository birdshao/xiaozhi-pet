# P0 开发板及上游软件基线

更新时间：2026-10-25。**以下静态核对不等于固件已编译或实物已测试。**

## 1. 已经完成的静态核对

| 项目 | 结果 | 证据 |
|---|---|---|
| ESP32 固件 upstream | `78/xiaozhi-esp32 v2.5.2` | GitHub tag SHA `49b4380b3faa0ceadd28a3e78aaf0f311c46c57e` |
| Python 语音服务 upstream | `xinnan-tech/xiaozhi-esp32-server v0.9.8` | GitHub tag SHA `8f2dda09cf18f1c1ece0f906eee4cc2fabb7fb58` |
| 工具链 | `espressif/idf:v6.1` | 上游对应版本的 `.github/workflows/build.yml` |
| 开发板路径 | `main/boards/waveshare/esp32-s3-epaper-3.97` | [小智板卡代码](https://github.com/78/xiaozhi-esp32/tree/v2.5.2/main/boards/waveshare/esp32-s3-epaper-3.97) |
| 官方构建入口 | `python scripts/build.py waveshare/esp32-s3-epaper-3.97 --name esp32-s3-epaper-3.97` | 上游 `config.json` 与 `scripts/build.py` |
| 屏幕 | 800×480，黑白，逻辑物理缓冲 48,000 字节 | 上游 `config.h` 与显示源码 |
| 板级按键 | 源码注册 `BOOT`、`PWR` | `waveshare-s3-epaper-3.97.cc` |
| RTC、IMU、SD | 硬件资料列出相关能力；当前是否完整接入未知 | 需实物、总线与软件验证 |

### 为什么版本采用固定 Commit？

小智固件与服务端不同步发版，且上游主分支经常变化。锁定 SHA 能使编译/错误复现不依赖某一天的 `main` 内容；`version-lock.json` 同时锁定两个项目。但这**不保证**两版本协议完全兼容，需要 P3 对接验证。

## 2. 可执行的软件基线检查

从此仓库根目录运行（Linux/macOS 或支持 Bash 的开发环境）：

```bash
python3 scripts/p0_check.py --metadata-only
python3 -m unittest discover -s tests -p 'test_p0_*.py' -v
bash scripts/p0_checkout.sh
```

`p0_checkout.sh` 默认将**未修改的官方固件**放在 `.upstream/xiaozhi-esp32`，对已存在目录拒绝覆盖。它核实 tag 对应的 commit 和板卡目录存在，不会烧录设备。

在已激活 ESP-IDF 6.1 环境下：

```bash
cd .upstream/xiaozhi-esp32
python scripts/build.py waveshare/esp32-s3-epaper-3.97 --name esp32-s3-epaper-3.97
```

CI 工作流见 [P0 upstream firmware baseline](../../.github/workflows/p0-baseline.yml)。CI 通过的含义只是“在 GitHub 的环境中可编译原版板型”，**不是**“硬件被成功烧录、宠物界面已集成或声学体验正常”。

不要直接把本工作流编译得到的原版固件当成儿童应用固件；它没有 Pet Engine、安全审核和家长管理。

## 3. 实物验证记录模板（尚未执行）

| ID | 测试 | 状态 | 证据路径/观察 |
|---|---|---|---|
| P0-01 | 实物板批次、接口、实际是否有拨轮 | 待测 | |
| P0-03 | 自己的板子烧录、上电、串口日志 | 待测 | |
| P0-04 | 联网、ASR、扬声器、打断、音量 | 待测 | |
| P0-05 | 全刷、局刷、睡眠唤醒 | 待测 | |
| P0-06 | BOOT/PWR、IMU 芯片 ID 与姿态事件 | 待测 | |
| P0-07 | SD 挂载与持续读取 | 待测 | |
| P0-08 | PSRAM、堆、任务栈、分区与功耗 | 待测 | |
| P0-09 | 上游集成方式 / 稳定合并策略 | 待定 | |

串口日志中不得包含 Wi-Fi 密码、真实身份信息、儿童录音、云端密钥。烧录前应确认端口、供电、电池及适配的板型。

## 4. 待验证的硬件风险

- 目前板级代码已确认 BOOT/PWR 回调，是否存在“旋转拨轮”要看实物，不将前期文字描述直接当成事实。
- SD 卡槽不代表固件已支持文件音频播放；需增加 SD 文件系统和 AudioCoordinator。
- 当前电子纸驱动有局刷接口，但此前对驱动的源代码检查表明，LVGL flush 仍以全幅缓存转换为主；P1 必须真机验证正确性及残影。
- ESP32-S3 PSRAM 是否足够支撑完整宠物 UI + 音频，必须根据编译产物/运行峰值判断。
- ESP-IDF 6.1 是 P0 固件上游 CI 的工具链基线，若本地 SDK patch 版本/依赖变化，需记录额外差异。

## 5. 本轮 P0 的边界

已完成：锁定 GitHub 上游版本、核对板卡源码/构建方式、提交自动预检与官方固件 CI 构建定义。

已经通过：[P0 GitHub Actions](https://github.com/birdshao/xiaozhi-pet/actions/runs/40187425730) 的 host 单测、静态校验和**未修改官方原版微雪固件的编译**；CI 已上传七天保留的固件产物。

尚未完成：原版硬件刷写、测量电子纸/音频/IMU/SD、选择最终源码集成方式、构建自己的儿童固件。
