# 第一本真实中文绘本：《小猫，快回来！》研究试点

状态：Book Dash 真实图像下载 + 逐页中文草稿 + 在线 TTS 可用性验证；**需要人工逐页试听、视觉审核后才能做孩子使用的 KPB2**。

## 为什么选这本

Book Dash 官方 `Come back, cat!`：
- 官方页面：https://bookdash.org/books/come-back-cat/
- 固定仓库提交：`54916310c5c06a5282e2d5d4add18d64f90e5fde`
- 原始 Markdown：`come-back-cat/en/index.md`
- 12 张插图，原始第 5 页、第 12 页确实没有正文；导入器保留这两页的无文字状态。中文版由改编者另行添加简短描述，不是原书原文。
- 原文：Nicola Rijsdijk；插画：Karen Lilje；设计：Sam Scarborough。
- 原作授权：CC BY 4.0（https://creativecommons.org/licenses/by/4.0/）；应注明原作者、作品、链接、改编和配音来源，不可暗示获得 Book Dash 背书。

中文稿：`curriculum/pilots/come_back_cat.zh_draft.json`，12 页；署名/许可配置：`curriculum/pilots/come_back_cat.credits.json`。它是 AI 辅助的**未审核改编稿**。语义、画面匹配、语速与发音必须人工核查。

## 1. 一条命令验证真实图片与 EPD 显示效果

在有网络的研究环境运行：

```bash
python -m pip install -r tools/story_pack_requirements.txt
python -m tools.learning_live_bookdash_pilot \
  --output-dir research_content/live_bookdash
```

该命令严格使用固定 Git 提交：

1. 下载原始 `index.md`，验证 UTF-8、来源 Markdown SHA256、12 页（含 5/12 两页无文字）。
2. 注入来自官方 Book Dash 的固定作者/插画/设计者信息（不是任意推断）；将 12 页中文草稿与原文按页绑定。
3. 从原始 `raw.githubusercontent.com/bookdash/bookdash-books/<SHA>/...` 拉取 12 张 JPEG，并保存原图 SHA256，不自动抓取别的站点。
4. 用纯音调**测试素材**代替中文音频，显式标为 `test_only`；音调不可用于儿童内容。
5. 输出 12 页 800×480 图，使用现有 KPB2 内核生成 12 张黑白和 12 张四级灰度预览。
6. 写出 `live-report.json`，但**不会生成 .kpb**，不会设置 approved，也不会修改板端固件。

输出均在被 `.gitignore` 排除的 `research_content/`。

GitHub Actions 测试：`.github/workflows/learning-live-bookdash.yml`。在线图片测试使用真实上游图像，单元测试仅使用模拟图像；不要混淆两者。

## 2. 生成整本真实中文朗读音频

需另装可选的 `edge-tts` (7.3.0+) 和 `ffmpeg`：

```bash
python -m pip install 'edge-tts>=7.3.0,<8.0'
ffmpeg -version
python -m tools.learning_live_bookdash_pilot \
  --output-dir research_content/live_bookdash_real_voice \
  --narration edge
```

这会重新从官方固定提交获取 12 页原图，再对 12 页中文草稿执行真实在线 TTS，生成 24 kHz / 单声道 / PCM16 WAV 并检查逐页文件，关联原文与图像 SHA，形成审阅素材。

- `status=draft`：音频是**真实的中文朗读**，但**尚未人工审核**。不生成 KPB2 包。
- `status=test_only`：默认模式是纯正弦测试音调，不能当作中文朗读。
- 外部 Edge TTS 不是有 SLA 保证的云 API；运行前需检查服务条款、生成内容的可使用范围和当前可用性。
- 不要上传个人数据、孩子录音或私密信息。不要因为 WAV 格式检测成功就推断语义/发音质量可靠。
- 在线 TTS 在 GitHub CI 中作为**独立非阻断实验**，可能因云端限制失败；必须查看对应 job 的日志和 `live-report.json`，不能单纯依据整个 workflow 绿色来断言成功。

## 3. 审核与正式打包（后续人工操作）

阅读器审核工位：
```
research_content/live_bookdash_real_voice/
  source/source.md
  source/original.lcir.json
  pilot/curated.en.lcir.json
  pilot/come_back_cat.zh.lcir.json
  pilot/attribution.json
  assets/images/page_01.jpg ... page_12.jpg
  assets/audio/page_01.wav ... page_12.wav
  assets/audio.manifest.json
  assets/images.manifest.json
  assets/assets.json
  staged/book.json
  staged/attribution.json
  staged/candidate.json
  staged/preview/page_01_mono.png ... page_12_mono.png
  staged/preview/page_01_grey.png ... page_12_grey.png
```

先逐一检查：12 页中文是否与图片对应、黑白回退能否识别猫/篮子/蝴蝶/蜗牛等主体；真人试听全部 12 段配音，确认儿童安全与自然语速；确认作者/插画/设计者及二次改编标注。

审核**签署之前**，必须确保没有伪造审核人身份，且相关音频使用许可满足研究分发目的。参考 `docs/LEARNING_CONTENT_PIPELINE_QUICKSTART.md` 生成独立、绑定候选 `checked_hash` 的审核文件后，才运行 `build-book`：

```bash
python -m content_pipeline.cli build-book \
  --stage-dir research_content/live_bookdash_real_voice/staged \
  --review-file research_content/reviews/cat_review.json \
  --output research_content/builds/books/come_back_cat.kpb
```

程序**不自动创建审核文件**，不能通过 CI 将 `reviewed=true` 设置为假。正式 KPB2 在人工确认前不存在。最后还须在 Waveshare ESP32-S3 ePaper 3.97 的 TF 卡上测试，重点核对真实刷新、拨轮输入、所有页播放和开机断电恢复。

## 4. 相关代码

- `content_pipeline/source_snapshots.py`：固定原文 Git SHA 的 Markdown 下载和来源哈希。
- `content_pipeline/adapters/bookdash.py`：允许原始无文字图片页；不能用虚构文本填充原作。
- `content_pipeline/pilot.py`：试点专属官方署名与中文草稿绑定。
- `content_pipeline/assets.py`：真实逐页 JPEG 下载和 SHA256。
- `content_pipeline/localize.py`：保留源摘要的 EN→ZH 逐页改编。
- `content_pipeline/speech.py`：真实中文 TTS 和音频格式/来源校验。
- `content_pipeline/packaging.py`：黑白/灰度预览与人工审核门禁。
- `tools/learning_live_bookdash_pilot.py`：一条命令集成实验，**只生成待审核素材**。
- `tests/test_learning_content_real_pilot.py`：离线 12 页结构、版权和安全门禁测试。
