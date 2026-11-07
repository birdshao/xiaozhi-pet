# LCP-09：15 道数学题的固件级 KPG1 / 拨轮 / 声音 / 1-bit 画面验证

本阶段将 **LCP-04/08 的实际 15 道数学题**输入固件已实现的 C++ 游戏引擎，不另外模拟一个游戏客户端。实现可在无 ESP32 板卡的 Linux PC/GitHub Actions 上运行。

## 1. 当前验证到哪一层

| 层 | 状态 | 实现和边界 |
|---|---|---|
| 教学内容数据 | 已实现 | 5 形状、5 数数、5 大小比较，生成标准 KPG1 GameArt IDs |
| Python KPG1 二进制 | 已测试 | 使用原有 `tools.game_pack.make_pack`，CRC 和头部/音频表由工具回读 |
| **原有 C++ GamePackReader** | 已测试 | 验证 15 rounds、19 clips、CRC、错误数据拒绝、24kHz PCM16 分块读取 |
| **原有 C++ GameSession** | 已测试 | 32 组随机种子三题会话；正确反馈、两次错误揭晓、一次重复提示、60 秒退出 |
| **原有 InputHub** | 已测试 | 拨轮向下、短按确定、中心长按回家，防抖后再处理；限于软件级别 |
| **原有 SceneComposer** | 已测试 | 从真正 C++ GameArt 绘制 800×480 P4 黑白帧、导出 PNG |
| Edge 中文 TTS 到真实 C++ Reader | 可选联网测试 | 临时 placeholder KPG1；不允许把未审内容当成正式包 |
| **实机 GPIO / SD / I2S / 电子纸刷新** | **尚未完成** | 需要连接授权的 BIRDSHAO 板卡并实际采集图像/听感 |

这是对固件原有逻辑的测试，不是运行自己的替代渲染器。固件仍从 `/games/default.kpg` 读取一个游戏包；每局通常抽 3 道题，15 道只是题池。KPG1 本身没有设备端课程菜单。

## 2. 一条命令测试原生固件逻辑

仓库根目录 Linux/WSL（需要 `g++`、Python 3.10+、Pillow）：

```bash
python -m pip install -r tools/story_pack_requirements.txt
bash scripts/lcp09_device_host_test.sh
```

过程：
1. 从 `original_math_lesson()` 取得 15 道真实结构的课题。
2. 生成 **19 段合成测试音调**，沿用 LCP-08 严格校验和音频清单。
3. 从未审核的 `game.json` 构造只存在于临时测试目录的 encoder 输入；用现有工具生成 `placeholder=true` 的 KPG1。
4. 用 C++17 编译 `GamePackReader`、`GameSession`、`InputHub`、`SceneComposer` 等原有固件文件；执行 `tests/cpp/test_learning_math_device_flow.cc`。
5. 导出 **15 道真实固件题目黑白画面 + 5 张不同 UI 状态**，P4 PBM 转 800×480 PNG。
6. 为成人输出 `previews/index.html`，展示真实 1-bit 固件像素、每题原始中文题干、选项与正确答案。图片反映包内选项原始顺序；真机游戏会随机打乱选项。

测试输出：

```text
build/lcp09/
  device-fixture/
    device-fixture-report.json
    README-TEST-ONLY.txt
    source/lesson.json
    stage/game.json                  # approved=false
    games/default.kpg               # TEST ONLY; not distributable
    games/default.manifest.json     # placeholder=true
  true-gameart-previews/
    math_round_01_1bit.pbm ... math_round_15_1bit.pbm
    math_shape_1bit.pbm
    math_count_1bit.pbm
    math_size_1bit.pbm
    math_selected_1bit.pbm
    math_right_1bit.pbm
  previews/
    index.html                       # 可本地浏览 15 张真实题目画面
    *.png                            # 20 张 800×480 图片
    device-scenes.json
```

**注意**：纯音调包不等于经过成人审核的正式语音教材，不要复制到儿童设备。GitHub Actions 仅上传 PNG/HTML 和测试报告，不上传这个 `.kpg`。打包器临时源字段的 `approved=true` 仅为运行原有二进制 encoder 的 CI 夹具，不是任何人的人工批准；真正给孩子使用的 `game.json` 始终 `approved=false`，正式审核流程见 LCP-08。

## 3. 验证真实中文语音的 native 路径

有网络的研究电脑可运行：

```bash
python -m pip install 'edge-tts>=7.3.0,<8.0' 'imageio-ffmpeg>=0.6.0,<1.0'

LCP09_NARRATION=edge bash scripts/lcp09_device_host_test.sh \
  build/lcp09-real-voice
```

上述过程将真实 Edge TTS 的 19 段中文 PCM16 输入临时 `placeholder=true` KPG1，再由固件原有 C++ 播放路径分块读出并验证内存边界。**不代表已在 Waveshare 音频硬件上真实播放，也不是儿童内容审核**；系统级扬声器音量、延迟、语速、语义仍需专门验收。

GitHub Actions 工作流：
- `.github/workflows/learning-math-native.yml` 的离线 C++ job 为稳定回归，必须通过；
- 另有独立在线 Edge job，是外部服务依赖的非阻断实验，会明确报告真实 outcome；不上传真实音频或临时 `.kpg`；
- `.github/workflows/p1-scene.yml` 是现有固件通用场景回归，应一并关注。

## 4. 实机验收清单（此阶段**未执行**）

在 BIRDSHAO 通过 Remote Desktop Commander 重新授权连接后：

- **设备/启动**：原版固件和合法 SD 分区可启动，识别 TF 卡中经过**真人审核**的 `/games/default.kpg`；卡不存在或损坏时可以稳定退回内置小游戏。
- **触控/拨轮**：左右/上下（三向）切换选项，短按确认；中心长按约 1 秒回首页；没有重复奖励、长按意外执行和自动连点。
- **显示**：物理电子纸上用相同题目观察 GameArt 形状是否可辨，选项框是否清晰，局刷后有无严重残影；确认 800×480 没有旋转/倒置。
- **音频**：确认题干播报、选项切换朗读、正确与错误反馈；观察 24kHz PCM16 的缓冲耗尽、切换时延、音量与声音破音。
- **游戏体验**：3 岁用户不需识字可以听声操作，正确时鼓励、错误时不挫败；适当的 3 题单次时长，60 秒闲置返回主页。
- **安全边界**：无人签署的包不能作为教学内容发布，任何纯音调 `placeholder=true` 的 CI 文件不得安装为正式课程。

完成实体硬件检查后才应该进入 LCP-10：课程主题选择、多包热切换和实际孩子学习进度；不能将 C++ 主机测试冒充板卡测试。
