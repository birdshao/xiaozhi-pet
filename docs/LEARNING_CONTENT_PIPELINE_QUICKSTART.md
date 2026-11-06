# Learning Content Pipeline：离线导入、草稿整理和 KPB2 审核构建

当前实现：LCP-01、LCP-02、LCP-03 的**人工图片/音频接入部分**。
这不是联网下载器、TTS、批量课程自动导入器或已通过儿童审核的绘本。

## 0. 安装与安全边界

在仓库根目录安装既有绘本依赖：

    python -m pip install -r tools/story_pack_requirements.txt

所有原始绘本、录音、图片和导出包放到根目录 `research_content/`；该目录由 .gitignore 排除，不要把研究用途第三方原始素材意外提交到公开仓库。

- 支持 Python 3.10/3.12；所有导入测试与构包测试都不依赖网络。
- 导入器只读**本地 Markdown**，不会访问源站；需要先从上游仓库固定提交 SHA 取出文件并自行保存。
- 全部作者、插画作者、译者、图片实际来源和许可证逐一核对。实验的非商业目的不等于获得公开再分发许可。
- 批量自动生成的中文翻译和音频是**待审核草稿**，不能跳过人工审核。

## 1. 导入 Global ASP 中文 Markdown

例如把 pinned upstream `zh/0008_你在做什么.md` 存到本地：

    research_content/sources/global-asp/0008.md

运行（将全 40 位 SHA 替换为实际锁定的 Git commit）：

    python -m content_pipeline.cli import-global-asp \
      --input research_content/sources/global-asp/0008.md \
      --source-path 'zh/0008_你在做什么.md' \
      --ref <40位Git提交SHA> \
      --content-id 1001 \
      --revision 1 \
      --skill language.action_verbs \
      --license-id CC-BY-3.0 \
      --registry research_content/catalog/ids.json \
      --output research_content/normalized/asp-0008.json

许可证字段必须人工依据该作品的完整来源条款核实；如果尚未确认，可不传 `--license-id`，导入结果保留 `unknown` 并且不能正式构建。

Global ASP Markdown 正文未包含插图文件；导入器仅生成页文本和原作者/插画/翻译者信息，不捏造对应图片。

## 2. 导入 Book Dash 图片故事结构

从 Book Dash 的 pinned commit 中获取 `book-slug/en/index.md`：

    python -m content_pipeline.cli import-bookdash \
      --input research_content/sources/bookdash/index.md \
      --source-path 'book-slug/en/index.md' \
      --ref <40位Git提交SHA> \
      --content-id 1002 \
      --skill language.story_sequence \
      --license-id CC-BY-4.0 \
      --registry research_content/catalog/ids.json \
      --output research_content/normalized/bookdash-1002.json

Book Dash `index.md` 中的 `{{ site.image-set }}/01.jpg` 被解析为 `image_ref = images/01.jpg`。这仅是来源图片引用，不是已下载、已确认许可或已渲染的图片文件。

Book Dash 旧 GitHub 仓库 Markdown 不一定包含原书作者和插画作者的完整信息，导入后应在 LCIR1 `source.creators.text` / `source.creators.illustration` 中人工补齐，不能省略署名。修改内容后若同 `item_uid` 维持同一个 `revision`，ID registry 将拒绝再次注册，必须递增 revision。

## 3. 检查导入草稿

    python -m content_pipeline.cli check \
      --input research_content/normalized/asp-0008.json

成功只表示 LCIR1 结构合法。导入器输出 `review.status=draft`，页面图片/音频为空，**不能作为儿童可播放内容**。

## 4. 手工准备图片与音频

把每页相匹配的图片（PNG/JPEG 等 Pillow 支持格式）和 WAV 放进单独目录，例如：

    research_content/assets/book-1001/
      page1.jpg
      page1.wav
      page2.jpg
      page2.wav
      assets.json

`assets.json`：

```json
{
  "schema_version": 1,
  "pages": {
    "1": {"image": "page1.jpg", "audio": "page1.wav"},
    "2": {"image": "page2.jpg", "audio": "page2.wav"}
  }
}
```

页号必须与 LCIR1 全部页一致，不能缺页或多页。可使用任何合法、经审核的图片，但其与对应文字的含义必须由人工核对；不能用一张任意“占位图”冒充真实绘本插图。

音频须为 WAV、24 kHz、单声道、PCM16，时长 0–60 秒；先用外部可信 TTS/录制与音频工具做好。此阶段还**没有**自动 TTS。

## 5. 生成审核工位（不会打包）

    python -m content_pipeline.cli stage-book \
      --input research_content/normalized/asp-0008.json \
      --assets research_content/assets/book-1001/assets.json \
      --output-dir research_content/stage/book-1001

输出：

- `page_01.png`、`page_02.png` 等：统一 800×480，等比缩放+白边；之后由已有 KPB2 工具生成黑白/灰度帧。
- `page_01.wav`、`page_02.wav`：已验证基本格式的音频副本。
- `book.json`：页面和全书仍为 `approved=false`，不能直接通过 `story_pack --reviewed` 编译。
- `lcir.json`、`attribution.json`：来源、译者/插画者、原始素材 SHA-256 记录。
- `candidate.json`：绑定整个审核工位主要元数据和文件字节的 `checked_hash`。

这一步只做格式转换，不代表图片在实际墨水屏上清晰，也不能代替逐页试听。

## 6. 人工审核与构建 KPB2

审核者须逐页查看文字、图片、音频及其来源授权，特别确认 1-bit 回退帧适用于黑白电子纸。随后**手工**创建 `review.json`（脚本不会自动生成）：

```json
{
  "status": "reviewed",
  "reviewer": "实际审核者姓名或称呼",
  "checked_hash": "把 candidate.json 的 checked_hash 完整复制到此处",
  "attestations": {
    "source_license": true,
    "illustrations": true,
    "text": true,
    "audio": true,
    "age_safety": true
  }
}
```

构建：

    python -m content_pipeline.cli build-book \
      --stage-dir research_content/stage/book-1001 \
      --review-file research_content/reviews/book-1001-review.json \
      --output research_content/builds/book-1001.kpb

`build-book` 重新哈希所有 staged 资产并与候选哈希比较，要求五项人工确认均为 true，拒绝未知许可证或缺少原作者/插画者的图书，之后使用仓库现有的 `tools.story_pack.make_pack` 生成 KPB2。

按既有工具再验证并预览：

    python -m tools.story_pack validate \
      --package research_content/builds/book-1001.kpb

    python -m tools.story_pack preview \
      --package research_content/builds/book-1001.kpb \
      --output research_content/preview/book-1001

真正验收时还必须将包复制到 TF 卡 `/books/`，上真实 Waveshare 3.97 墨水屏检查每一页 1-bit 画面、朗读时长、声音质量和操作。

## 7. 当前实现范围

已实现：
- Global ASP 分段/元数据解析；Book Dash Markdown 图文配对；
- LCIR1 严格校验、源文件哈希、锁定源 Git SHA、稳定 ID registry；
- 图片等比居中到 800×480、WAV 格式校验、素材引用目录检查；
- 待审核 KPB2 内容工位、素材哈希、五项人工审查和正式构包；
- 使用项目原本的 KPB2 encoder 和 parser，CI 做真实二进制 round trip。

尚未实现：
- 网络发现/批量抓取、Global ASP 图像定位、Book Dash 文件自动同步；
- 自动翻译、内容分级、题目生成和配音；
- 批量创建 KPB2 页内选择题、GCompris 五类题到 KPG1；
- 多游戏包菜单、Learning Engine 学习进度、线上 S4 内容目录与分发；
- 真实 ESP32 设备端的验收。

后续建议优先 LCP-04（GCompris 规则模板与 KPG1），同时开展现有绘本图片下载器和中文 TTS 插件的技术预研，不直接修改 PetCore。
