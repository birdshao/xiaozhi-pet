# LCP-06：真实绘本插图与批量语音生产（仍需人工审核）

已实现：固定源 Git SHA 下载 Book Dash 插图、EN→ZH 逐页翻译草稿、Edge TTS 可选中文/英语口播、生成 `assets.json`，衔接既有 KPB2 和 KPG1；自动化测试**完全离线**（图片/声音均为合成 fixtures）。本功能不修改 ESP32 固件，也不绕过现有成人审核。

## 依赖与安全边界

- Python 3.10 或 3.12；
- `python -m pip install -r tools/story_pack_requirements.txt`，以提供 Pillow 图片解码；
- 真正在线语音生成才需要 `python -m pip install edge-tts` 和本机 `ffmpeg`（必须在 PATH）。`edge-tts` 调用在线服务，并非稳定的商业 API；请自行核对服务条款、声音使用和输出再分发条件。不上传孩子的录音或其他个人信息。
- 真实图片下载需要可访问 `raw.githubusercontent.com`；必须使用 40 位上游 Git commit，不能填 `master`/`gh-pages` 浮动分支。仅下载 PDF 之外的 `images/*.jpg`，限制文件字节数与尺寸，不允许外部 URL、重定向、路径穿越。
- 所有生成资产默认放仓库 gitignored 的 `research_content/` 中。下载/语音生成成功**不是审核通过**；公开仓库只放代码和合成测试样例。
- Book Dash 旧仓库说明和图文格式：https://github.com/bookdash/bookdash-books 。其 `_data/meta.yml` 有作品级创作者及授权信息；每本仍需对照实际条目核实。大多数素材采用 CC BY 4.0，但不要丢掉作者、插画者、译者和变更说明。

## 1. 取本书的原始 Markdown 和固定 Git 提交

Book Dash 旧内容仓库 `gh-pages` 的示例提交（2026-11-06 查看）：

    54916310c5c06a5282e2d5d4add18d64f90e5fde

任选一本短篇。以 `a-tiny-seed/en/index.md` 为技术演示（**原文本较长，不代表已经适合 3–5 岁**）。从该提交取出 Markdown 到：

    research_content/sources/bookdash/a-tiny-seed.md

导入英文结构：

```bash
python -m content_pipeline.cli import-bookdash \
  --input research_content/sources/bookdash/a-tiny-seed.md \
  --source-path a-tiny-seed/en/index.md \
  --ref 54916310c5c06a5282e2d5d4add18d64f90e5fde \
  --content-id 1101 \
  --license-id CC-BY-4.0 \
  --skill language.story_sequence \
  --registry research_content/catalog/ids.json \
  --output research_content/normalized/a-tiny-seed.en.json
```

`import-bookdash` 解析每一个 `![... ]({{ site.image-set }}/01.jpg)` 图片引用及后续段落。结果仍为 `review.status=draft`，`source.creators` 里的 writer/illustrator 如没有在 index.md 中出现，须从上游 `_data/meta.yml` 核对并人工填入。图片下载器不会为了凑数自动赋予不存在的作者信息。

## 2. 逐页生成中文版草稿（不能省略译文审核）

此步骤不依赖一个不可靠的自动翻译服务。你可以用自己的 AI 批量翻译，但必须先提供**与英文页面一一对应**的中文字句 JSON。示例仅示意 schema，不是上述长篇绘本的完整翻译：

```json
{
  "schema_version": 1,
  "title": "示例中文版标题",
  "translator": "译文草稿负责人",
  "pages": ["第一页中文句子。", "第二页中文句子。"]
}
```

`pages` 数量必须和原文的图片数相等；每页 1–320 字符，不能错位；不允许把英语源文当成中文送进晓晓配音。中文文件例如 `research_content/translations/a-tiny-seed.zh.json`。执行：

```bash
python -m content_pipeline.cli localize-bookdash \
  --input research_content/normalized/a-tiny-seed.en.json \
  --translations research_content/translations/a-tiny-seed.zh.json \
  --content-id 1301 \
  --registry research_content/catalog/ids.json \
  --output research_content/normalized/a-tiny-seed.zh.json
```

转换会保留英语原文的 Git commit、图片路径、著者和作品许可证；新增 `zh-Hans`、译者、原文摘要 `source_digest`，仍标为 draft。生成器不自动判定译文质量。

## 3. 自动按 Git SHA 下载插图

```bash
python -m content_pipeline.cli fetch-bookdash-images \
  --input research_content/normalized/a-tiny-seed.zh.json \
  --output-dir research_content/assets/a-tiny-seed
```

输出 `images/page_01.jpg` 等和 `images.manifest.json`。URL 由工具构造为：

    https://raw.githubusercontent.com/bookdash/bookdash-books/
      <40-digit-commit>/a-tiny-seed/en/images/01.jpg

工具验证原始 JPEG 可解码、大小在限值内，记录每页来源 URL 与 SHA256。它**不**自行寻找 Global ASP 的缺失插图，因为其 Markdown 没有明确图片路径，不能凭书名猜图与乱拼页。

如需要重新获取，需明确传 `--replace`，并重新人工核对旧的审核哈希。

## 4. 自动生成中文口播音频

安装可选语音依赖与 ffmpeg 后执行：

```bash
python -m content_pipeline.cli tts-book \
  --input research_content/normalized/a-tiny-seed.zh.json \
  --output-dir research_content/assets/a-tiny-seed \
  --voice zh-CN-XiaoxiaoNeural
```

工具用 `edge-tts` 生成临时 MP3，再用 ffmpeg 明确转成 WAV：24kHz、单声道、PCM16，按页验证采样率、位深、时长与 SHA256。输出：

    audio/page_01.wav
    audio/page_02.wav
    ...
    audio.manifest.json

英语原版可选择 `en-US-AriaNeural`；中文稿必须选 `zh-CN-...Neural`。音频仍是**未审核**。人必须完整试听发音、停顿、错别字、过快语速、儿童安全内容，必要时换 TTS 或亲自录音。

## 5. 拼接并验证 KPB2 输入

```bash
python -m content_pipeline.cli prepare-book-assets \
  --input research_content/normalized/a-tiny-seed.zh.json \
  --assets-dir research_content/assets/a-tiny-seed

python -m content_pipeline.cli stage-book \
  --input research_content/normalized/a-tiny-seed.zh.json \
  --assets research_content/assets/a-tiny-seed/assets.json \
  --output-dir research_content/stage/a-tiny-seed
```

`prepare-book-assets` 将检查图片和口播清单是否来自同一 LCIR1 摘要，页次、源路径、每页文字 SHA256 与实际文件字节匹配，否则拒绝。它只生成 `assets.json`；正式 KPB2 构建继续按 `docs/LEARNING_CONTENT_PIPELINE_QUICKSTART.md` 的人工审核门禁执行。

如果是 CI 的合成音调，语音 `status=test_only`：正常 `prepare-book-assets` 和正式 `build-book` 都会拒绝，禁止把音调样例当成适龄绘本。

## 6. 自动生成数学游戏中文语音

上一阶段 LCP-04 的 15 道数学题可以直接复用自己的 TTS 管线：

```bash
python -m content_pipeline.cli make-math-game \
  --output-dir research_content/game_math

python -m content_pipeline.cli tts-game \
  --input research_content/game_math/lesson.json \
  --output-dir research_content/game_math \
  --voice zh-CN-XiaoxiaoNeural

python -m content_pipeline.cli stage-game \
  --input research_content/game_math/lesson.json \
  --audio-map research_content/game_math/audio_map.json \
  --output-dir research_content/stage/math-game
```

`tts-game` 按中文句子内容去重配音（多道题共用同样的提示和选项读音），写 `clips/*.wav`、`audio.manifest.json`、`audio_map.json`。音频在 KPG1 的 0.1–10 秒范围内，否则拒绝。所有 WAV 的实际字节 SHA256 绑定审核工位，未通过人工五项复核不能执行 `build-game`。

如果 `audio.manifest.json` 声明 `test_only`，正常游戏构建会拒绝；纯音调 CI 测试必须显式 `smoke_only=True`，并将输出 manifest 标记 `placeholder=true`。

## 7. 本轮验证和未完成的工作

- 离线 unit test 使用自己生成的 JPEG 与纯音调 WAV 模拟完整素材流：固定 Git SHA、图文对应、中文翻译、音频生成、哈希篡改检测、WAV 格式及失败处理、KPB2/KPG1 stage。
- 在线真实 Book Dash 下载需在有网络的研究 PC/runner 上执行；CI 测试**不冒充**真实网络下载。
- Edge 服务可用性、中文配音质量、第三方语音许可和真机墨水屏呈现，尚待人工验证。
- 图片下载仅支持 Book Dash 的 Markdown 逐页 JPEG；Global ASP 的插图查找、英文自动翻译/分级、课程按主题选择、真实孩子课程试用和 S4 服务器分发仍未实现。
