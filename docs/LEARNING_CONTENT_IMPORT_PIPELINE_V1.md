# Learning Content Pipeline V1：开放教育资源导入与课程索引设计

状态：设计草案，待评审；不含生产代码、不改变固件协议。
日期：2026-11-05
项目：xiaozhi-pet（面向 3–5 岁儿童，ESP32-S3 ePaper 800×480）

## 0. 决策摘要

**先做可追溯、可审核的内容生产线，再开发自适应学习引擎。**

V1 不新建板端格式，不复制第三方 Qt/QML 应用进 ESP32，不直接将 AI 生成内容投喂儿童。将现成教育资源解析成独立于 KPB2/KPG1 的统一中间格式 LCIR1，再分别由现有工具编译为 KPB2 绘本和 KPG1 规则游戏；学习目标、版权与课程关系保存在旁路索引中。

这样可以先大量利用现有内容，且不影响 PetCore、MemoryStore、现有板端状态机及其回归测试。

### 实施边界

- V1 目标：内容采集、结构化、图文转换、TTS、审核、编包、预览、内容来源追踪、课程索引、自动化测试。
- V1 非目标：让孩子直接使用实时生成的 AI 内容、语音给分、学习水平诊断、动态多游戏包切换、新的 KPG2、服务端内容分发或重写 PetLearning 数据表。
- 现有服务端 S4 尚在规划中；本设计是 S4 上游的离线生产链，产物可交给 S4 接管，但不声称已经联网安装。
- 只要内容来源、文字、插图或音频发生变化，审批必须失效，重新进入审核。AI 只能产生候选，不允许自动设置 approved=true。

## 1. 工程事实（2026-11-05 核验）

| 项 | 当前事实 | 设计处理 |
|---|---|---|
| KPB2 | 最多 32 页；每页 0–2 题，最多 64 题；2–3 个带音频图标选项 | 初期将短绘本和页内选择题编译成 KPB2 |
| 画面 | 800×480，每页有 1-bit 回退；可带 4 级灰度帧 | 生成两套预览；真实设备当前主要用 1-bit 回退 |
| KPB2 声音 | WAV 输入为 24 kHz/mono/PCM16；页最大 60 秒，题干 15 秒，选项 5 秒 | TTS 统一重采样并静音裁剪；每段按限制检测 |
| KPG1 | 至多 64 题、128 声音片段、8 MiB；五类游戏 | 不支持的游戏先存于 LCIR1，不强行编译 |
| KPG1 游戏类型 | match_shape、count、animal_sound、big_small、english_word | 设计精确映射，不复制需要拖拽/颜色屏的玩法 |
| 图库 | 内置 circle/square/triangle/star/heart、dots1–5、blocks1–5、big/small ball、cat/dog/bird/fish；listen 仅题目图 | 图库没有的选项不能生成 KPG1，改走 KPB2 或进入待支持 |
| 加载 | 固件主要尝试 /sdcard/games/default.kpg，单次游戏只做少量轮次 | V1 用一个可重建的精选聚合包；按课程选包需要以后改固件 |
| 审核 | story_pack / game_pack 编译器要求 approved、reviewer、--reviewed；旁路 manifest 带 SHA | 生成器永不自签；由可信家长审核后显式编包 |
| 设备学习状态 | 现有 PetMemory 仅有宠物喜好/成长类记忆 | LCIR1 只表达知识目标；暂不把成绩混入 PetMemory |

对应现有代码：
- tools/story_pack.py，tools/kpb2.py，tools/game_pack.py
- docs/V4_KPB2_CONTENT_FORMAT.md
- docs/V4_MINI_GAMES.md
- docs/V4_CONTENT_REVIEW_CHECKLIST.md
- docs/V5_V6_SERVER_PLAN.md（S4 内容分发与 sha256 审核）

## 2. 内容来源适配器与优先顺序

### 2.1 P0：Global ASP 中文故事（首选中文验证）

- 项目：https://github.com/global-asp/global-asp ，中文文件位于 zh/*.md。
- 源文档按 ## 分页，以末尾 License/Text/Illustration/Translation 元数据标注作者与许可。
- 已检查中文目录的 README：共有 78 条故事，54 条 CC-BY、24 条 CC-BY-NC（依当前目录，不代表未来保持不变）；逐文件核对许可。
- **图片不在所读的 md 正文里**，图片查找、取回与页序对齐独立为 ImageResolver，未匹配到授权图片时拒绝生成完整绘本。
- 初期挑选题材适龄、语句简短、许可明确的 2 本。示例：《你在做什么？》《一个很高的人》；选材还要经过中文自然度与插图质量审核。
- 每一篇都固定来源路径 + Git commit SHA + 文件 hash，不从不稳定的网页猜测图片路径。

### 2.2 P0：Book Dash 故事（优先绘本源文件）

- 官方重用说明：https://bookdash.org/re-using-the-book-dash-content/。
- Markdown/图片示例：https://github.com/bookdash/bookdash-books/tree/gh-pages/a-tiny-seed/en。
- 可识别 index.md 中图片次序与段落，也可将官方源文件作为手工导入输入。GitHub 旧仓库并不代表全部 200+ 本均为同一结构。
- 将原文保留、中文译文作为派生版本，保存翻译责任人/审核者；不得标为原作者中文原作。
- 先试 1 本低龄短书（优先简短句、较少文字、明确页面结构），再扩大。
- Book Dash 图书官方以 CC BY 4.0 为主，实际条目仍保存单本许可证与署名说明。

### 2.3 P1：GCompris 游戏活动

- 官方代码：https://github.com/KDE/gcompris
- 不是将 QML/Qt 程序移植到固件：先研究关卡规则、教育目标、难度梯度，再在 LCIR1 中表达。
- 初始五个模板：形状匹配、数数、动物叫声、大小比较、英语听词选图。
- 优先自行生成几何图案、重新配音；原始代码、图片和声音的许可各自核对，特别是 GPL/AGPL 等互惠要求。
- 只有能映射到当前五类 GameKind 和内置 GameArt 的题目才编译 KPG1；其他活动记录 blocked_reason=unsupported_game_kind/art，并保留以待 KPG2。

### 2.4 P1/P2：StoryWeaver、Global Digital Library

- StoryWeaver https://storyweaver.org.in/en/open-content ：故事及插画 CC BY 4.0；Readalongs/视频另属 CC BY-NC-ND 4.0，不能当作同一授权批量二次编辑。
- 先做人工导入的 EPUB/PDF/图片+文字路径；除非官方公布且验证了批量接口和使用条款，不假定存在稳定 API。
- GDL https://content.digitallibrary.io/api/ ：优先做格式和授权元数据探针，测试资源可访问性；具体接口和覆盖率需另做 Spike，不能把源站所有内容当可自由转发。
- 所有源站必须尊重 rate limit、robots、访问条款；不绕过登录、DRM、付费限制。

### 2.5 研究用途与公开仓库的分离

- **可以在本地研究**并不代表可以将任何网络素材提交到公开 GitHub 仓库。
- 原始文件库、转换中间件、TTS WAV、预览和编译后的 .kpb/.kpg 默认保留在 gitignored 工作目录。
- 仓库中仅存脚本、课程索引规范、可公开的测试夹具、授权来源记录及原创演示内容。
- 对可再发布素材，仍保持作者、插图、译者、授权版本、原始来源和“经过修改”的声明；旁路归档与家长端可查。未知许可默认 quarantine。

## 3. LCIR1：统一课程中间格式

目标：**教学语义与板端二进制格式解耦**。

一门课程由多个教学单元组成；内容项是绘本或练习，知识技能通过稳定 ID 关联多个来源。

目录建议：

    content_pipeline/
      adapters/
        global_asp.py
        bookdash.py
        gcompris_activity.py
        storyweaver_file.py
      lcir/
        schema_v1.py
        validate.py
        id_registry.py
        provenance.py
      render/
        epaper.py
        tts.py
        icons.py
      exporters/
        kpb2.py
        kpg1.py
      review/
        checklist.py
      cli.py
    curriculum/
      skills.v1.json
      courses/
        shapes_and_numbers.v1.json
    research_content/           # .gitignore，本地自动生成
      sources/
      normalized/
      draft/
      reviewed/
      builds/
      preview/

### 3.1 LCIR1 顶层字段

| 字段 | 必选 | 约束与用途 |
|---|---|---|
| schema_version | 是 | 固定 LCIR1，不等于 KPB2 schema_version=2 |
| item_uid | 是 | source:source_id:locale:variant，跨构建稳定；修改内容递增 revision |
| kind | 是 | picture_book 或 mini_game；预留 hands_on、audio_only |
| title, locale, age_band | 是 | 目标语言与适龄标签，非能力诊断 |
| source | 是 | provider、url、source_ref、license_id、creators、asset hashes |
| skills | 是 | 如 math.count_1_5 / language.action_verbs；有版本化目录 |
| pages 或 rounds | 至少一类 | 绘本页面/游戏题目结构，统一携带文本、资源和练习 |
| content_id, revision | 打包前必选 | 显式注册，不由导入顺序动态分配 |
| review | 是 | draft/reviewed/rejected；reviewer、checked_hash、checked_at、notes |
| transformations | 是 | 翻译、裁切、TTS、灰度转换等处理版本和人工修改说明 |

### 3.2 建议的 JSON 约束

- item_uid、skill_id 用小写拉丁字母、数字、点、下划线、连接符组成，最长 96 字符；locale 使用 BCP 47（如 zh-Hans、en）。
- content_id 是无符号 16 位非零整数；建议分区：书 1000–19999，游戏 20000–39999。不能因为源目录重排而改号，整个项目建立唯一 registry。
- revision 在内容、版式、音频或审核素材变动后递增。相同 item_uid + revision 的编译输入 hash 必须相同，否则报冲突。
- 题目 answer_key 只能指向已有选项；偏好题需严格遵循 tools/kpb2.py 现有 like_topic 白名单；普通知识题不写喜好记忆。
- 所有引用的相对路径必须限制在源工作目录内，禁止 ..、绝对路径及指向目录外的 symlink；大小、总页数、图片像素、音频时长在编包前限制。
- license_id 支持 CC-BY-3.0、CC-BY-4.0、CC-BY-NC-3.0 等精确值；未知许可必须标注 unknown，不能由提供方或站点总许可无条件代替单项许可。
- 仅在 LCIR1 中放来源/审校信息，不向设备端发送作者隐私、审核者私人标识或儿童个人信息。

示例见同分支 docs/LEARNING_CONTENT_LCIR1_EXAMPLE.json。样例是**原创占位结构**，不代表素材实际下载、审核或具备可用音频。

### 3.3 课程文件和技能表

将课程编排与原始内容分离，例如 shapes_and_numbers：

    course_id: preschool.shapes_and_numbers
    age_band: 3-5
    units:
      - unit_id: shape_recognition
        skills: [math.shapes_basic]
        items: [book:... , game:...]
      - unit_id: number_1_5
        skills: [math.count_1_5]
        items: [book:... , game:...]

课程文件可保存同一技能的跨绘本和游戏关联，但 **V1 只是离线索引**；不能假称现有设备已能依据 skill 选择内容、跟踪正确率或自动调度复习。

## 4. 可复现导入-审核-编包流水线

    [allowlisted source / local import]
                    |
          discover + pin revision
                    |
         fetch + verify hashes
                    |
          parse to LCIR1 draft
                    |
          classify age + skills
                    |
       translate / split / question draft
                    |
     render 800x480 + mono/grey previews
                    |
       generate/resample 24k PCM16
                    |
        static validation + attribution
                    |
       human review (assets + audio)
                    |
    signed-off content hash / reviewed
                    |
    exporter to existing book.json/game.json
                    |
    story_pack/game_pack --reviewed
                    |
    read-back validate + SHA256 + preview
                    |
          local research SD or S4 draft

原则：每步输入输出带 hash 和工具版本，失败可重试；相同输入+配置产生一致的编译输入，导入器不能修改上游源文件。

### 4.1 导入器的最小接口（建议）

    class SourceAdapter:
        def discover(self, query) -> list[SourceItem]: ...
        def fetch(self, item, cache_dir) -> SourceSnapshot: ...
        def normalize(self, snapshot) -> LCIR1: ...

V1 必须支持离线 fixture 运行。在线 discover 不作为测试必需条件。每个 source snapshot 保存 manifest.json，包括出处和逐文件 sha256。

### 4.2 页面排版

1. 先保留来源插图，不默认生成新图；按主体安全区域（文字外另设空白、裁切不能损失主体）调整到 800×480。
2. 不能简单对任意纵横比居中裁切；提供 contain+留白、轻裁切、人工焦点三种策略，默认 contain。
3. 对 4 级灰度和 1-bit 抖动结果分别预览，实机阶段优先验收 1-bit 可辨识度，避免把彩色教材原本含义完全丢失。
4. 中文页文字 1–3 简短句；不能机械按原书每页对齐，必要时拆页，但每包 ≤32 页、每页朗读 ≤60 秒。插图与句子对应必须人工核查。
5. 题目叠图/高亮不能遮挡插图主体；KPB2 每页最多 2 题、选项 2–3，位置坐标满足 8 px 对齐与不重叠校验。
6. 由于板端屏幕不能区分彩色，纯颜色辨认题应标记需外部实物或不适配，不能把颜色题强行转黑白题。

### 4.3 题目转换规则

- 仅将答案确定且适龄的题目作为知识题；趣味讨论/开放式问答保留在课程旁路注释，不把没有唯一答案的题目编成单选。
- GCompris 规则模板可以产生原创的数数、大小、形状、动物声/英语词题，但必须匹配 KPG1 KINDS + ART；否则输出 rejected_export_reason，并允许人工选择 KPB2 页面问答。
- 绘本页建议最多 1 道核心题；其余内容放到单独复习游戏，避免每页强制打断朗读。
- 错误选项不能构成歧义：如大小比较需要相同尺度前提；英语听词与选项要逐一复核。
- AI 可建议知识点和题目，绝不能将 AI 自认的正确答案视为已审核答案。

### 4.4 TTS

- 输入必须是最终已审文稿，不能在 TTS 后暗中改台词。
- 统一输出 24000 Hz、mono、16-bit PCM WAV（打包器剥 WAV 头）。
- 自动检验采样率、位深、峰值、前后静音、爆音、时长和文件 hash；音量归一化后需要重新校验。
- 朗读/提示音用许可明确的 TTS 引擎/声音；绝不使用第三方盗版声音克隆；不上传儿童录音到制作流水线。
- 家长必须实际试听所有待发布音频。研究版可缓存生成的 WAV，公开仓库默认不提交。

## 5. 审核与安全：明确的状态机

    discovered -> fetched -> normalized -> drafted -> review_pending
                                                       | approved by adult
                                                       v
                                                     reviewed -> built -> validated
                                                       |                   |
                                                       +-- content changed --+
                                                                           |
                                                                      review_pending

- 审核依据是文件集合 SHA256/规范 JSON hash，不是单独 approved 布尔值。
- source license 不明确：quarantined；素材无法正确配图：blocked_missing_image；年龄不符：rejected_educational；格式不支持：blocked_unsupported_feature。
- 允许先生成 draft PNG 和 WAV 供审阅，禁止在未审核时调用打包器传 --reviewed。
- 研究素材工作目录与公开仓库分离；仅可公开许可证明确且署名完整的内容。
- 现有 KPB2/KPG1 CRC 是完整性校验而非第三方信任凭证；未来接入 S4 时必须按服务端已审核的整包 sha256 绑定分发，不信任卡上的自称 approved。

## 6. 批量内容与当前固件差距

### 6.1 KPG1 单文件聚合器

现状：设备主要读取 /sdcard/games/default.kpg，一局玩 3 题（包可含更大题池）。V1 先输出可重建的 selected_game.json，其中只包含经过审核并可映射的轮次，合并后仍满足 64 rounds/128 clips/8 MiB 约束。

需要稳定排序/固定种子、跨题共享音频去重、限制单一主题占比、打包后逐条校验。不宣称能在设备界面里选择多套课程。

### 6.2 下一代设备端改造（V2 才立项）

- 读取只读 curriculum_index，经可信存储/sha256 校验；
- 课程单元选游戏包，再通过菜单在多包间切换；
- 学习进度单独持久化，按技能记录“独立/提示后/待练习/跳过”，不写进现有 PetMemory；
- 能力层只负责出题与复习建议，不接管 PetCore 奖励、作息、安静时段与电源；
- KPG2 仅在现有五类游戏确实不足时再设计；新增图形素材等能力应先分析电子纸刷新与 RAM 约束。

## 7. CLI 建议（均为**待开发命令**）

    python -m content_pipeline.cli sources list
    python -m content_pipeline.cli import --provider global-asp --ref <commit> --id zh/0008_你在做什么.md
    python -m content_pipeline.cli normalize --item <uid>
    python -m content_pipeline.cli render --item <uid>
    python -m content_pipeline.cli tts --item <uid> --engine <configured>
    python -m content_pipeline.cli check --item <uid> --offline
    python -m content_pipeline.cli review-export --item <uid> --output review_bundle/
    python -m content_pipeline.cli build --item <uid> --review-file trusted_review.json
    python -m content_pipeline.cli preview --package research_content/builds/<id>.kpb

以上不是当前可运行的功能。在 V1 真正开发时保留干运行模式，不依赖联网完成 unit test。review-file 必须关联具体 sha256，不能只由脚本自动填写 reviewed=true。

编包出口优先调用**已有**命令：

    python -m tools.story_pack pack --source book.json --output out/book.kpb --reviewed
    python -m tools.story_pack validate --package out/book.kpb
    python -m tools.story_pack preview --package out/book.kpb --output out/preview
    python -m tools.game_pack pack game.json --output out/default.kpg --reviewed
    python -m tools.game_pack verify out/default.kpg

## 8. 实施拆分、验收标准

### PR LCP-01：LCIR1 数据契约 + 来源追踪

- 严格 schema 校验、来源许可记录、内容 ID 注册、课程 skill 字典；
- 纯本地测试夹具、拒绝路径穿越/失效许可/重复内容 ID；
- 验收：单元测试无需网络、无第三方版权二进制入库。

### PR LCP-02：Global ASP + Book Dash 最小导入器

- 各导入真实文本结构及页次；图片 resolver 对不上必须中止，不造图/乱填图片；
- 原始文本与译文并存；支持 pinned Git SHA 和本地缓存；
- 验收：各至少 1 个有效 snapshot -> LCIR1；重复执行 hash 相同；作者/译者/许可无丢失。

### PR LCP-03：画面/TTS/KPB2

- 800x480 contain 渲染，双模式预览；TTS 24k PCM16；
- 生成符合已有工具输入的 book.json，人工审核后才编包；
- 验收：至少 1 本 4–8 页真实内容可经 story_pack 验证；逐页试听、实机 1-bit 显示检查。

### PR LCP-04：GCompris 教学模板 + KPG1

- 先做 count、match_shape、big_small 三类，另两类待音频库准备；
- 图库严格匹配 GameArt，生成原创新题与合法音频；
- 验收：至少 15 道通过 game_pack verify，聚合资源不超限，设备一局能跑完 3 题。

### PR LCP-05：课程目录、批量构建与审核工位

- 增加课程单元/skill 的映射、查询、审核检查表、包/来源 SHA 索引；
- 验收：3 本中文绘本 + 15 道游戏的最小体验闭环，离线可用且审阅清晰；
- 只合并审核流程与代码，不自动提交第三方素材；S4 分发另按既有服务器计划实施。

## 9. 首轮课程样板：形状与数量

| 单元 | 技能 ID | 候选来源 | 板端承载 | 注意 |
|---|---|---|---|---|
| 认识高矮长短 | math.size_compare | Global ASP《一个很高的人》 | KPB2 | 图片与中文译文人工复核 |
| 数数 1–5 | math.count_1_5 | 自制几何图、GCompris 数数规则 | KPG1 count | 数量画面只用 dots1–5 / blocks1–5 |
| 认识基础形状 | math.shapes_basic | 自制几何图、GCompris 规则 | KPG1 match_shape | 圆/三角/方图形现成 |
| 动作词 | language.action_verbs | Global ASP《你在做什么？》 | KPB2 + 动作任务 | 每页不超过 1 个问题 |
| 雨天观察 | science.weather_rain | Global ASP《雨》 | KPB2 | 非屏幕任务由家长陪同，不自动判定正确性 |

V1 不追求一次生成 50 本书。建议先 3 本中文短绘本 + 15 道游戏作为端到端 POC，真实设备验收后再扩至 20 本/100 题，再考虑 50 本/300 题。

## 10. 先确认的技术问题与风险

1. Global ASP 插画是否有可稳定批量取回、且与中文版页序对齐的上游仓库/API；无则先人工提供图像文件。
2. Book Dash Markdown 旧仓库覆盖面有限；新来源官方素材可能是 PDF/InDesign，另做解析适配，不把旧仓库当全部图书。
3. 中文 TTS provider 的声音许可、发音质量、费用与本地离线可行性需要单独定型。
4. KPG1 仅支持固定图库，且单次 3 题和一个默认包；大课程随机出题可能重复，需要后续设备改动。
5. 现有 EPD 一次全刷约 4 秒，对高频切题/问答不利，编排时每页问题不要太密。
6. 内容包自动生成后如未进行真机声画审核，不得标记内容生产目标完成。
7. 低龄适宜性、文化/译文自然度、语言难度、隐私及正确答案审核仍须人工参与。

## 11. 项目来源与规范链接

- GCompris：https://github.com/KDE/gcompris
- Global ASP 中文：https://github.com/global-asp/global-asp/tree/master/zh
- Book Dash 版权/源文件：https://bookdash.org/re-using-the-book-dash-content/
- StoryWeaver 许可：https://storyweaver.org.in/en/open-content
- GDL 内容 API：https://content.digitallibrary.io/api/
- 项目现有 KPB2/KPG1 工具与测试：见本文件第 1 节

下一步优先实施 LCP-01 与 LCP-02，不修改固件，必要时在本地研究目录手工提供有明确许可证的插图，然后串接已有 story_pack 做首本真实中文绘本。
