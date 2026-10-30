# P4 Waveshare 3.97：真实 TF 卡挂载与黑白书页显示

此 PR **已经在实际使用的 `scripts/p1_integrate.py` 中接线**，不是只增加一个无人调用的读取类。

## 固定硬件映射与证据

微雪官方仓库 `waveshareteam/ESP32-S3-ePaper-3.97` 的
`ESP-IDF/04_SD_Test/components/sdcard_bsp/sdcard_bsp.c` 对此板明确配置 SDMMC 4-bit：

| 信号 | GPIO |
| --- | ---: |
| SD_CLK | 16 |
| SD_CMD | 17 |
| D0 | 15 |
| D1 | 7 |
| D2 | 8 |
| D3 | 18 |

官方仓库来源：
https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/main/ESP-IDF/04_SD_Test/components/sdcard_bsp/sdcard_bsp.c

已锁定的小智板卡 `config.h`：EPD 用 SPI3 + GPIO9/10/11/12，ES8311 I2S 为 13/14/21/47/48，音频输出 24 kHz。当前 SDMMC 引脚配置与上述已有 EPD/I2S GPIO 不重复。

## 实际接入的行为

- 进入 Pet 模式并初始化事件/显示任务时，`WaveshareStorySd::Initialize("forest_friends")` 使用 ESP-IDF SDMMC 4-bit、保守默认时钟与 FAT VFS 将 TF 卡挂载在 `/sdcard`。
- **绝不格式化**：`format_if_mount_failed=false`；不存在卡、挂载失败、没有 `/sdcard/books/forest_friends.kpb`、或文件结构/CRC 校验失败时，继续显示原有阅读页占位图，不破坏普通宠物功能。
- 点击 BOOT 双击进入 Reader 后，`PetRender` 走真正的 `StoryPackReader::ReadFrame`，读出第 1 页已校验的 800×480 / 1-bit 内容；BOOT 单击取下一页；超出 KPB1 实际页数时使用原占位画面。
- 仅专用 EPD worker 访问文件并加载页图；单次为整页创建 PSRAM MonoFrame，FAT 读取发生在 LVGL 锁之前；屏幕写入仍走原来的 `pet_screen_mutex_` + `lvgl_port_lock` + `EPD_DisplayPartBaseImage`，由现有刷新门控限速。
- Pet 模式退出仍走 `ExitPetPreview`，正常小智 LVGL 界面不被永久覆盖。
- 只读包使用已有 KPB1 CRC 校验；`approved` 与 CRC 不等同于可信签名，内容必须由家长审核并按受控设备的 SD 介质管理。

## 怎样准备测试卡

先从已经完成的 `tools/story_pack.py` 制作合规 `forest_friends.kpb`。需要家长审核所有文字、图片、24kHz PCM16 单声道 WAV 后加 `--reviewed`。将包复制到 TF 卡（FAT32）的 `/books/forest_friends.kpb`，插在开发板的 TF 卡槽里。

编译验证：P1 integrated app-only GitHub Actions。它只构建固件文件、不烧录。C++/Python 源码检查验证实际接线与引脚；最终显示效果必须在实体板卡上确认。

## 未完成与限制

**这版仅接通离线书页画面，尚未通过扬声器播放 PCM 音频。** 小智原有声音输出由 `AudioService` 统一管理，直接调用底层 ES8311/I2S 的 `OutputData` 会与原有播放/唤醒词任务争用，不能贸然这样接。音频后续必须由同一音频任务序列化或增加明确的安全隔离协议。

未验证实际 TF 卡热插拔、硬件 SDMMC 信号完整性、反相/屏幕方向、实际刷新耗时以及 TF 卡缺失后的再次插入；本轮不做固件刷写和 OTA。当前先采用固定演示书籍 `forest_friends`，后续再由家长白名单与实体拨轮选择图书。
