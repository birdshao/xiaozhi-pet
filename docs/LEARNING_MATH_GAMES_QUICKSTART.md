# LCP-04：形状、数量与大小比较游戏（KPG1）

实现日期：2026-11-06。状态：Python 规则游戏生成、音频清单、审核与编包；**未完成中文 TTS、真实儿童内容审核和真机测试**。

## 目标和来源

这一阶段把通用的低龄数数、图形配对等教育活动转换为 Xiaozhi Pet 固件现有的 **KPG1** 5 种 GameKind 中的 3 种。GCompris 的相关活动用于理解教学类型和难度；**本实现没有复制其 Qt/QML 源码、图像、录音或数据文件**，而是自己写了固定题型及中文提示。GCompris 源代码与媒体素材涉及不同许可证，未来如果真的读取其源数据，还须建立独立的逐文件许可清单。

参考：
- https://github.com/KDE/gcompris/tree/master/src/activities/enumerate
- https://github.com/KDE/gcompris/tree/master/src/activities/babyshapes
- 本仓库 `docs/V4_MINI_GAMES.md`、`tools/game_pack.py`

**现状限制**：
- KPG1 固件现有的可选图片只有内置 `GameArt`；没有任意彩色图片、拖拽和拼图。
- `/games/default.kpg` 是当前主要读取路径；有 15 道题的题池**不等于**可以按 15 个课程菜单选择，设备默认从题池抽少量题（当前默认 3 道）。
- KPG1 支持至多 64 rounds、128 clips、8 MiB；声音必须是 24 kHz、单声道 PCM16 WAV，长度 0.1–10 秒。
- 游戏编译器接受的 `approved=true` 是审核者显式授权，不由生成器设置。

## 一、首批 15 道题

| 类别 | 数量 | 题目画面/选项 | 需要孩子做什么 |
|---|---:|---|---|
| 形状匹配 match_shape | 5 | 圆/方/三角/星/爱心 | 找和上面相同的形状 |
| 数数 count | 5 | 题目图为 1–5 块积木，答案为 1–5 个圆点 | 数一数并选择一样多 |
| 大小比较 big_small | 5 | 图片为大球/小球，题目有不同问法 | 找大球或小球 |

说明：大小比较的五道是两种概念（大与小）的语言变式，不是五种不同数学技能。题目正确答案、题干和图片组合由 `content_pipeline/game_lessons.py` 确定性生成，`validate_lesson()` 检查答案是否与题目含义一致，不依赖 AI 判断孩子答案。

本阶段中间规范是严格的 **GAMELESSON1 草稿**，不是 LCIR1 通用绘本规范；它是向 KPG1 迁移的暂时子格式。完整课程索引和 Learning Engine 之后统一治理，当前只承载游戏题目，避免过早修改已有 LCIR1 代码。

## 二、命令一：生成草稿及 TTS 文稿

```bash
python -m content_pipeline.cli make-math-game \
  --output-dir research_content/game_math/
```

将生成：
- `lesson.json`：GAMELESSON1、15 道题、技能标记、答案和内置图形；status=draft。
- `audio_script.json`：按口播文字去重的 `clip_key -> 要说的话` 清单。
- `audio_map.json`：`clip_key -> clips/clip_<hash>.wav` 的预期本地文件路径。

例如，多个图形题共用“找出和上面一样的形状。”，只需要一段题干录音；多道题的“圆形”答案名也只需要一段录音。稳定 `clip_key` 以文字的 SHA-256 前缀生成；文本有变化就会对应新的录音需求，避免旧录音误配。

**需要另行准备真实中文语音**：对 `audio_script.json` 中每段文稿用家长录音或授权 TTS 生成相应 WAV，存至 `research_content/game_math/clips/`，要求 24kHz、单声道、PCM16、每段 0.1–10 秒。代码不进行自动 TTS，CI 中的合成正弦波只是验证 KPG1 文件结构，绝不可当作适龄朗读音频。

## 三、命令二：检验草稿并建立审核工位

```bash
python -m content_pipeline.cli check-game \
  --input research_content/game_math/lesson.json

python -m content_pipeline.cli stage-game \
  --input research_content/game_math/lesson.json \
  --audio-map research_content/game_math/audio_map.json \
  --output-dir research_content/stage/math-game/
```

输出：
- `game.json`：符合 `tools.game_pack` 字段格式，但所有 `approved=false`。
- `lesson.json`、`attribution.json`：原题和具体每段 WAV 的 source SHA-256。
- `clips/*.wav`：拷贝后的 WAV，按 KPG1 严格校验格式/时长。
- `candidate.json`：文本、资源和审核工位哈希与 `checked_hash`。

这一阶段 **没有** `.kpg`。即使草稿全部写着正确答案，也不能直接使用 `tools.game_pack pack --reviewed` 打包。

## 四、命令三：人工确认后构建

负责人需试听**每句题干、每个选项名称**，审查答案、难度及来源，手工在工作目录新建 `parent_review.json`（绝不可由脚本自动填写）。

```json
{
  "status": "reviewed",
  "reviewer": "实际审核人称呼",
  "checked_hash": "复制 stage/math-game/candidate.json 的 checked_hash",
  "attestations": {
    "educational_accuracy": true,
    "answer_uniqueness": true,
    "audio": true,
    "age_safety": true,
    "source": true
  }
}
```

然后执行：

```bash
python -m content_pipeline.cli build-game \
  --stage-dir research_content/stage/math-game/ \
  --review-file research_content/reviews/parent_review.json \
  --output research_content/builds/games/default.kpg

python -m tools.game_pack verify \
  research_content/builds/games/default.kpg
```

工具先重新哈希所有草稿和 WAV，并检查所有审核声明，再调用仓库原有的 `tools.game_pack.make_pack` 编码（不是自己编写新的 KPG1 格式）。正式输出必须名为 `default.kpg`，才能符合当前板端默认读取路径。

任何题目或音频发生变化，都必须重新审核。CI 中的合成提示音测试构包时强制 `placeholder=true`；这只是验证二进制文件的可靠性，不是可以提供给儿童的音频内容。

## 五、测试与限制

自动测试 `tests/test_learning_content_games.py` 覆盖：
- 15 道题的数量与三种类型分布，全部选项属于固件已定义 GameArt；
- 错题、错误选项名称、不一致问句和答案、重复选项/未审核状态拒绝；
- 口播词去重与稳定音频文件名；
- 音频缺失、16kHz 错误采样率、逃逸的软链接、错误输出文件名拒绝；
- 人工审核信息错误、候选 hash 失效、原音频被篡改拒绝；
- 使用合成测试 WAV 构建 KPG1，然后由原 `game_pack.parse()` 回读 15 道题；
- 以不同 Python 版本运行。

仍待后续：
1. LCP-05：课程索引与不同内容包选择；目前设备只默认加载一个 `default.kpg`。
2. 中文 TTS 与批量插画导入：真实语音素材、录音质量/来源、幼儿实测。
3. 对 GCompris 的更多教学活动进行**规则层**筛选，如数字排序、记忆配对，不能强行塞入目前不支持的题型。
4. 按用户真实硬件做灰度屏、拨轮、声音的端到端验收。
