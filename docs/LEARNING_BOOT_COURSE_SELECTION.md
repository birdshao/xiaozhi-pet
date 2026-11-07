# LCP-10：三门数学启蒙课独立 KPG1 与 TF 启动选课

**状态（2026-11-07）：** 已实现固件 C++ 选择器、Waveshare BSP overlay、课程拆包与主机 CI 验证；**尚未完成真机刷写和实体 TF 卡／墨水屏／扬声器测试**。此处的“选课”是**家长修改 TF 卡文本、下次启动生效**，并非孩子在屏幕上即时切换课程。

## 1. 启动后选择哪门课

TF 卡预期目录（仅示例，真实教学包必须经过成人审核）：

```text
TF_ROOT/
  games/
    active.txt          # 纯 ASCII 文本：例如 counts + 换行
    shapes.kpg          # 5 道形状题，content_id=20011
    counts.kpg          # 5 道数数题，content_id=20012
    sizes.kpg           # 5 道大小比较题，content_id=20013
    default.kpg         # 可选的经过审核的回退题池
```

允许的 `active.txt` 内容是**一个 3–15 字符**的 ASCII 文件名主体：小写 `a-z`、数字 `0-9`、非首字符 `_` 或 `-`，可选一个 LF/CRLF 换行，不包含扩展名。不得写路径、空格、中文、第二行或 BOM：

```text
counts
```

上电只在 `pet_prompt` 任务读一次 `active.txt` 和相应 `.kpg`；原有 `SoundPackFilePort` 打开 KPG1，`GamePackReader::Open` 检查完整 CRC、大小、题目和录音引用，然后把不可变的读取器交给原有 `GameSession` / 音频任务。

**退路顺序：** 选中的包正常 → 选中的包缺失/损坏则尝试 `default.kpg` → 默认也缺失/损坏则使用固件内置小游戏。选课文件缺失、非法时直接尝试默认包。**不会自动信任未审核内容，也不会在运行中热替换正在读取的 FILE\***。修改选课文本后必须重启。

KPG1 的 CRC **不是密码学签名**，只能查意外损坏；若要经网络下载课程，还需要 S4 的服务端审核／授权／完整性核对。

## 2. 生成三份待审核课程草稿

```bash
python -m tools.learning_math_unit_sources \
  --output-dir research_content/math_units
```

会创建：

```text
research_content/math_units/
  unit-sources.json
  shapes/lesson.json  # preschool_shapes, 20011
  counts/lesson.json  # preschool_counts, 20012
  sizes/lesson.json   # preschool_sizes, 20013
```

这些均为 **`review.status=draft`**，不含任何批准或儿童学习记录。各包只使用现有 GameArt 和自编数学题；每包必须**分别完成中文配音、试听、审查、哈希绑定审核及正式打包**。

## 3. 每包生产真实中文音频并接受成人审核

先准备联网研究环境：

```bash
python -m pip install -r tools/story_pack_requirements.txt
python -m pip install 'edge-tts>=7.3.0,<8.0' 'imageio-ffmpeg>=0.6.0,<1.0'
```

以 `shapes` 为例；对 `counts` 和 `sizes` 分别重复，不能把三份审核状态合并：

```bash
python -m content_pipeline.cli tts-game \
  --input research_content/math_units/shapes/lesson.json \
  --output-dir research_content/math_units/shapes \
  --voice zh-CN-XiaoxiaoNeural

python -m content_pipeline.cli stage-game \
  --input research_content/math_units/shapes/lesson.json \
  --audio-map research_content/math_units/shapes/audio_map.json \
  --output-dir research_content/math_stage/shapes

python -m content_pipeline.cli make-game-review-packet \
  --stage-dir research_content/math_stage/shapes

python -m content_pipeline.cli make-review-template \
  --kind game \
  --stage-dir research_content/math_stage/shapes
```

成人用本地浏览器打开 `research_content/math_stage/shapes/review/index.html`，试听全部题干和选项；核对答题唯一性、题图和录音对应、适龄性、来源与生成语音服务条款。审核模板中的 `status=review_pending`、`reviewer=""`、五项 `false` **不允许直接当作签署文件**。成人独立复制为 `research_content/reviews/shapes-review.json` 后完成真人签署和确认。

```bash
python -m content_pipeline.cli build-game \
  --stage-dir research_content/math_stage/shapes \
  --review-file research_content/reviews/shapes-review.json \
  --output research_content/built/games/shapes.kpg

python -m tools.game_pack verify \
  research_content/built/games/shapes.kpg
```

这个改造使 `build-game` 除 `default.kpg` 之外，还支持**与设备选择器一致的短 ASCII 包名**。测试包音调仍被 `smoke_only` 隔离，不能当作正式课程发布。翻译、发音质量和教学内容的检查不能被 CI 自动替代。

## 4. 生成安全的 TF 卡目录（只在本机工作区写文件）

当三门课程都已真实审核、产生 `placeholder=false` 的 `.kpg` 和 `.manifest.json` 后执行：

```bash
python -m tools.learning_course_sd \
  --course curriculum/courses/math_units.v1.json \
  --skills curriculum/skills.v1.json \
  --package research_content/built/games/shapes.kpg \
  --package research_content/built/games/counts.kpg \
  --package research_content/built/games/sizes.kpg \
  --active counts \
  --output-dir research_content/tf_math_courses
```

> 注意：`tools.learning_course_sd` 只在 `--output-dir` 指定的**本机新目录**里整理出 `/games/` 目录树（staging-only），不会写入 TF 卡；请人工检查后自行复制到卡上。每个包的旁路元数据必须带审核闸门写入的 `checked_hash`（仅 `--test-only` 例外），直接调用 `make_pack(..., reviewed=True)` 得到的包会被拒绝。

脚本会解析全部 KPG1 二进制、核对原始 `sha256`、审核旁路元数据与内容 ID，并生成含三单元的 `COURSEINDEX1`。所有检查先在临时目录进行，再原子移至目标文件夹；目标目录已存在则拒绝覆盖。

输出 `games/*.kpg`、`games/active.txt`、`course-index.json`、`card-report.json` 以及说明文件。**它不会直接格式化或写入已挂载 TF 卡，也不负责刷固件。** 家长自行核对后复制 `games/` 到 TF 根目录。

如果传 `--test-only`，脚本允许 CI 使用音调占位包，但输出状态会是 `test_only`，**绝不能用于孩子的正式教材**；不给 `--test-only` 时会拒绝这些包。

## 5. 在无硬件条件下的真实 C++ 验证

```bash
python -m pip install -r tools/story_pack_requirements.txt
bash scripts/lcp10_course_boot_host_test.sh
```

- 构建三个不同内容 ID 的**真实 KPG1 二进制测试文件**，每个 5 道题；另建原始 15 题的 `default.kpg` 作为回退夹具。
- 使用主机 `g++ -std=c++17 -Wall -Wextra -Werror` 编译真实的 `game_course_loader.cc`、`game_pack_reader.cc`、`mini_game.cc` 等固件源码。
- 检查 `active.txt` 的合法输入、危险路径、双行、CRLF、过长名称及乱码。
- 验证选择三包得到各自的 `20011/20012/20013`，每包均可以读取 PCM 音频并完成三题游戏；运行中修改选课文件不会热切换内存中的读取器。
- 验证选择缺失、损坏、CRC 异常、缺少默认包和根目录无效时的安全回退。
- 生成的测试音频全是**合成提示音**，并在 manifest 显式记录 `placeholder=true`。CI 不上传 KPG1 或 WAV，避免误用。

自动化：`.github/workflows/learning-course-selection.yml`；同时运行原有 P1/P2/P4 和内容流水线测试。开发机离线时仍可检验 C++ 逻辑和 BSP 源码的兼容性；**并不证明物理 SD 卡、I2S、拨轮 GPIO 或 EPD 已验收**。

## 6. 下一步：屏幕选课与学习进度

本版本没有做运行中热切换，避免音频播放与 KPG1 关闭时的竞态；未来屏幕选课应采用**停止会话 → 排空音频请求 → 释放旧文件 → 验证新包 → 重新开始新课程**的单任务切换协议，而不能直接由 UI 线程覆盖共享文件。

学习进度应与现有 PetMemory 的“完成了几次小游戏”解耦：按课程内容 ID/修订版本维护最近练习日期、练习完成次数和可选复习提示，不收集孩子的个人语音，不用一次答错判定能力水平。这个独立持久化能力尚未实现。
