# LCP-08：15 道数学启蒙游戏的真实中文语音与成人审核

状态：2026-11-06。本地实验与 GitHub CI 均支持真实 Edge TTS；内容仍是**未经成人审核的研究草稿**，不可直接给儿童使用或上传成正式 KPG1。

## 一、范围

本轮在现有 KPG1 游戏包之前增加真实可执行的音频与内容审核工位：

- 原创 15 道题：5 道形状配对、5 道数数（1–5）、5 道大小比较。
- 所有题目只使用**固件已有** `match_shape` / `count` / `big_small` 及已定义的图案标识，**没有复制 GCompris QML、图像或语音**。
- 题干和选项口播按中文文本哈希去重；使用 Edge TTS 及本地 ffmpeg 或 Python `imageio-ffmpeg` 转成 **24 kHz/单声道/PCM16**。
- `content_pipeline/game_packaging.py` 会逐轮核对题目语义、选项、标准答案、图标和对应录音，不能只检查文件哈希。
- `content_pipeline/audio_qc.py` 检查 PCM16 时长、全静音和异常音量：**全静音硬拒绝**，过低音量、削波、过长首尾静音等记录为建议检查事项，列入 `review-summary.json.technical_warnings`，不自动批准或判定发音正确。
- `content_pipeline/game_review.py` 生成只读审核网页，逐题展示问题、图标示意、正确选项与每条本地录音的试听控件，附 SHA/音频质量摘要。
- `tools/learning_live_math_pilot.py` 将这些步骤串成一条命令，但**不创建 `reviewed=true`，也不会生成 `default.kpg`**。

### 设备限制

当前设备固件读取 `/games/default.kpg`，并不支持按15个不同题目选择课程；一次游戏只抽若干题。目前网页中的 SVG 是**原创示意图**，并非黑白墨水屏上的实际 GameArt 像素，必须额外做真机校验。

## 二、一条命令运行真实中文语音

在仓库根目录：

```bash
python -m pip install -r tools/story_pack_requirements.txt
python -m pip install 'edge-tts>=7.3.0,<8.0' 'imageio-ffmpeg>=0.6.0,<1.0'

python -m tools.learning_live_math_pilot \
  --output-dir research_content/math_game_real \
  --narration edge
```

`--narration edge` 使用在线中文 `zh-CN-XiaoxiaoNeural`，需要网络。测试模式：

```bash
python -m tools.learning_live_math_pilot \
  --output-dir research_content/math_game_test \
  --narration synthetic
```

测试模式生成的是没有语言内容的音调，只用来检查文件结构。其 `status=test_only`，只能用于测试，不可自动拿去给孩子玩。

## 三、产物说明

```text
research_content/math_game_real/
  live-report.json
  README-RESEARCH-ONLY.txt
  generated/
    lesson.json
    audio_script.json
    audio_map.json
    audio.manifest.json
    clips/clip_<hash>.wav
  staged/
    lesson.json
    game.json
    candidate.json
    attribution.json
    clips/clip_<hash>.wav
    review/
      index.html
      review-summary.json
```

直接用浏览器打开 `staged/review/index.html`。按照页面顺序听题干和每个选项的音频，确认中文自然、语速适宜、答案唯一。网页顶部会提示声学技术警告数量，详细每段数值见 `review-summary.json`。网页不是正式设备 UI；正确答案只显示给审核者。

可在不改变音频素材的情况下重建审核页面：

```bash
python -m content_pipeline.cli make-game-review-packet \
  --stage-dir research_content/math_game_real/staged
```

如果录音、题目、答案或内容映射被修改，`candidate.json` 的原始审核 SHA 不再匹配，必须重新生成工作区再审核，不能复用旧的确认。

**生成待签署审核清单**（严格默认全部未通过）：

```bash
python -m content_pipeline.cli make-review-template \
  --kind game \
  --stage-dir research_content/math_game_real/staged
```

输出 `staged/review.template.json` 中，`checked_hash` 已绑定当前素材；但 `status=review_pending`、`reviewer=""`，五项 attestations 全是 `false`。直接将此模板交给正式 `build-game` 会失败。成人可以复制成独立的 `math-review.json`，在实际检查全部内容后填写真实姓名/称呼、改变状态并逐项确认。这个过程必须由人执行，不能由流水线自动批量替换为 true。

**切勿提交研究目录到公开仓库。** GitHub Actions 只上传 `live-report.json`、无音频的审核 HTML 和元数据，**不上传真实 WAV**。本地 HTML 的播放控件引用本地 `staged/clips` 文件；仅下载 CI 的元数据无法直接播放语音，需在联网研究 PC 上运行上述命令。

## 四、明确人工审核后的正式打包

在本地试听全部录音、检查题目和内置图标，以及核实 TTS 使用条款后，由真实审核人**自行创建** `research_content/reviews/math-review.json`，不得由自动化冒用审核人身份：

```json
{
  "status": "reviewed",
  "reviewer": "实际审核者填写",
  "checked_hash": "从 staged/candidate.json 手动复制最新 checked_hash",
  "attestations": {
    "educational_accuracy": true,
    "answer_uniqueness": true,
    "audio": true,
    "age_safety": true,
    "source": true
  }
}
```

五项必须由成人实际核验后确认，不允许批量自动通过。

```bash
python -m content_pipeline.cli build-game \
  --stage-dir research_content/math_game_real/staged \
  --review-file research_content/reviews/math-review.json \
  --output research_content/builds/games/default.kpg

python -m tools.game_pack verify \
  research_content/builds/games/default.kpg
```

这个打包器复用原有 KPG1 工具、保留成人审核的严格哈希匹配，不能由本轮研究脚本自动执行。

设备测试时将 `default.kpg` 放到 TF 卡的 `/games/`；必须再检查 3 题会话、拨轮选项、每道题显示和语音听感。

## 五、自动化测试与现状

- `tests/test_learning_content_math_live.py`：原创题列表完整性、音频去重、真正的 PCM/WAV 统计、语义/音频哈希篡改、全静音拒绝、错误答案拒绝、成人审核页面和 CLI、纯测试声音不能变成正式包。
- `.github/workflows/learning-live-math.yml`：在 GitHub Ubuntu 上使用可选 Edge TTS + `imageio-ffmpeg` 批量实际生成所有独立中文短句，验证 15 题与音频的完整映射，产物仍标记为 `unreviewed`。
- 外部 TTS 网络请求不属于可靠的必过 PR CI；若在线请求失败，将在工作流摘要中显示，不把失败误报为完成。
- Python 3.10/3.12 的离线测试完全不依赖网络，并覆盖声音质量检测算法。

尚未完成：真人逐条试听、儿童适宜性审核、真机墨水屏 GameArt 与拨轮体验、正式 KPG1 生成与部署；不要用 CI 合成成功直接替代这些验收。
