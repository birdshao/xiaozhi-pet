# Waveshare 3.97 TF 卡离线绘本：验收测试包（不是儿童资源）

用于已经完成的软件路径：BOOT 长按进入 Pet → 双击进入 Reader → CRC32 校验 KPB1 → 800×480 EPD 绘图 → 24 kHz PCM 通过小智唯一 AudioOutputTask → ES8311 扬声器。

## 生成及拷贝

在开发电脑上安装 `pip install -r tools/story_pack_requirements.txt`，再执行：

```bash
python -m tools.make_story_sd_fixture --output build/story-smoke
```

把 `build/story-smoke/books/forest_friends.kpb` **复制到一张 FAT32 TF 卡的 `/books/forest_friends.kpb`**。也可以从最新 `P4 offline SD story packages` Actions 构建中下载 `waveshare-tf-story-smoke-only` Artifact，解压保留 `books/` 目录后复制到 SD 卡。

测试包有两页明显不同的画面：第 1 页左侧大黑圆，440 Hz、约 0.5 秒轻声音；第 2 页右侧双黑条，659 Hz、约 0.5 秒轻声音。其 WAV 使用 24kHz、单声道、16bit PCM；每页内容应与视觉变化对应。黑白帧的编码和音频偏移由真实 `tools/story_pack.py` 生成。

## 验收顺序

1. 检查硬件接线、喇叭与音量，先把音量设置较低；确保旁边没有儿童，**该包是电子合成测试音，不是儿童故事**。
2. 将 TF 卡插入 Waveshare ESP32-S3 ePaper 3.97，使用项目自己构建并经你批准的 Pet 固件；仅插卡不会将新固件烧写到设备上。
3. 长按 BOOT 进入 Pet 模式，双击进入阅读器。验证墨水屏显示第 1 页左黑圆，同时喇叭播放短促 440 Hz；单击翻页，看屏幕变成第 2 页右双黑条并听到略高音调 659 Hz。
4. 阅读页双击退出，声音应停止；连续快速切页时，不应该出现旧页重复发声；小智进入 Listening/Speaking 时优先正常语音，故事音应取消。
5. 断网情况下重复 3–4（依赖本地 NVS 事件状态，不依赖 Python 故事 API）。
6. 拔出 TF 卡后需要重启再测试缺卡回退；**当前不支持热插拔**，不要在读卡过程中拔卡。
7. 记录串口 `PET_STORY_SD_FALLBACK`、`PET_STORY_AUDIO_UNAVAILABLE`、`PET_RUNTIME_ON` 以及真实屏幕与耳朵听见的结果。

## 限制

此 fixture 固定使用 `forest_friends` ID 是为了与当前板卡 demo 完全兼容；**不能将它作为家长审核的真实儿童内容**，也不能据此宣称 3–5 岁儿童的内容安全已经验收。固定二进制标志是软件格式兼容测试，不证明人类审核或内容真实性。CRC32 防随机损坏，不防恶意修改。

测试包仅供验证 TF 卡、EPD、I2S/扬声器以及 BOOT 翻页；真正的儿童故事仍需要家长审核每一页图片、中文文本、录制音频，并由 `tools.story_pack pack --reviewed` 单独制作。此工作流只输出文件，不访问硬件、不操作 OTA。
