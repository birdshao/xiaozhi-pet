# xiaozhi-pet — 儿童 AI 宠物与互动学习终端

> 项目状态：**main 已包含固件宠物功能、服务端 S0–S5/S7、家长 Web 面板、V8 养成/绘本/小游戏/声音、学习内容流水线与语音固定命令；真机验收与开放问答（S6）、语音自动测试、语音体验优化仍在进行。**  
> 文档基线：2026-11-08（对应 main `5bda7c0`）  
> 目标用户：**3–5 岁儿童（家长参与与控制）**  
> 硬件：**Waveshare ESP32-S3-ePaper-3.97**（800×480 墨水屏，三向拨轮，姿态/时钟/温湿度传感器，锂电管理，ESP32-S3）

## 项目定位

将“小智 AI”开源语音终端扩展为一只**能聊天、能互动、有持续状态、能陪孩子读绘本和学习**的墨水屏虚拟宠物。

不做“提示词 + 几张表情”的玩具；宠物行为、成长、画面、绘本进度、离线音效、家长控制由自己掌握，大模型负责适龄的语言能力，不决定底层关键操作。

### 技术路线

1. **设备固件**：基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，保留网络/音频/设备基础设施；增加隔离的 C++ 儿童应用组件（`firmware/`）。
2. **语音服务**：自建 [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)，ASR/LLM/TTS 采用可替换的本地或云端提供商，并叠加儿童守卫 overlay。**自建不等于全部模型本地部署**。
3. **儿童业务服务**：`services/pet_learning/`（Python），负责宠物长期状态、内容库、学习进度、配对、家长设置、审计和家长面板；语音服务通过边界明确的适配器调用。
4. **本地离线优先**：基础宠物交互和缓存故事不能依赖服务器在线；网络断开时降级，不丢失成长数据（原子账本）。
5. **维护策略**：固定上游版本（`version-lock.json`）、少量适配层、兼容性测试和自控 OTA。

**官方小智云的定位**：硬件/语音排障的参考基线，不是本项目的正式业务依赖。

## 任务看板（GitHub Issues）

按里程碑推进，详细子任务及验收勾选在各 Issue 内：

- [#1 P0：硬件及上游基线](https://github.com/birdshao/xiaozhi-pet/issues/1)
- [#2 P1：宠物 UI 与墨水屏刷新验证](https://github.com/birdshao/xiaozhi-pet/issues/2)
- [#3 P2：离线宠物 MVP](https://github.com/birdshao/xiaozhi-pet/issues/3)
- [#4 P3：自建 AI 语音与宠物联动](https://github.com/birdshao/xiaozhi-pet/issues/4)
- [#5 P4：绘本、学习与养成](https://github.com/birdshao/xiaozhi-pet/issues/5)
- [#6 P5：家长管理、OTA、安全与长期维护](https://github.com/birdshao/xiaozhi-pet/issues/6)

Issue 与新阶段（V 系列）的对应关系见 [开发计划 §4](docs/DEVELOPMENT_PLAN.md#4-旧阶段与新阶段对照)。

## 快速入口：部署、刷机、配对

| 要做什么 | 先看这里 |
|---|---|
| **部署服务端**（pet-learning + 语音服务 + 公网入口） | [部署记录](docs/DEPLOY_PET_SERVER.md)（PVE 实际部署）· [S7 运维手册与验收清单](docs/SERVER_S7.md) · [公网入口](deploy/ingress/README.md) |
| **刷机 / 升级固件** | [首次刷写与恢复门禁](docs/hardware/FIRST_FLASH_PLAN.md) · [OTA1 试验记录](docs/hardware/OTA1_BOOT_TRIAL_2026-11-02.md) · [BIRDSHAO Runner 联调](docs/hardware/BIRDSHAO_RUNNER.md) · [官方基线](docs/hardware/OFFICIAL_BASELINE_20261025.md) |
| **设备配对**（设备 6 位码 ↔ 家长账号） | [服务端 S1：配对与设备凭证](docs/SERVER_S1.md)（家长在 `/parent/` 面板或 JSON API 兑换码） · [家长 Web 面板](docs/PARENT_PANEL.md) |

> 刷写、OTA 写入和任何改动 Flash 的操作，都需要先按文档确认备份与恢复步骤，并获得用户授权。

## 当前进度（2026-11-08）

以下按 main 上已合并的 PR 归纳，详细验收状态见 [开发计划](docs/DEVELOPMENT_PLAN.md)。

### 已完成（已合并到 main）

- **固件基线**：纯官方 v2.5.2 固件已在真机 OTA0 启动（[记录](docs/hardware/OFFICIAL_BASELINE_20261025.md)）；默认 OTA 指向自建 pet server（#97、#98、#112）。
- **宠物固件**：板上启动直接进入宠物模式（#79）；拨轮、姿态、时钟、温湿度、电源管理（#57、#62、#63）；首页状态栏（#56）；哄睡关机与选项语音播报（V2 自动化试验，[记录](docs/hardware/V2_HARDWARE_TRIALS.md)）。
- **体感与内容**：体感反应与作息（#54）、环境小常识（#73）、灰度绘本 KPB2 与阅读器（#60、#66）、规则游戏（#67）、体感游戏（#72）、麦克风游戏（#74）。
- **个性与记忆**：三种性格、有界记忆、只从拨轮写入的喜好（#55）、记忆驱动话术（#76）；原子账本（#64）。
- **V8 养成与玩法**：共享活动列表（#102）、声音与作息计时（#105）、绘本升级（#106）、迷你游戏（#107）、照顾与成长（#108）。
- **服务端**：S0 迁移与配置（#78）、S1 配对（#92）、S2 状态同步（#94）、S3 初始化与策略（#99）、S4 内容分发（#93）、S5 语音意图与页面问答（#91）、S7 部署包与运维（#95）、公网 HTTPS 入口（#101）。
- **家长面板**：`/parent/` Web 管理端（#104）。
- **语音固定命令**：设备端分发（#77），经服务端 `self.pet.request` 到板子执行（#114）。
- **学习内容流水线**：LCIR1 导入、KPB2 审核、三门数学课程 KPG1、真实中文配音与板端选课（#80–#90）。
- **自测**：真机自测修复与 V8 场景（#109–#111）。

### 进行中

- **语音自动测试**：语音测试框架，[PR #113](https://github.com/birdshao/xiaozhi-pet/pull/113) 尚未合并。
- **开放问答（S6）**：GLM 直连、三道开关、TTS 儿童模式审计等，分支 `claude/laughing-bell-efcd2b`，尚未开 PR，文档未合入 main。
- **语音体验优化**：TTS 短句、唤醒词、延迟优化。

### 待做（需要真机或用户操作）

- 电池供电下实体 PWR 键、定时开机在 USB 供电下复测、4 灰度刷新与残影（见 [V1 验收](docs/hardware/V1_HARDWARE_ACCEPTANCE.md)）。
- 喇叭实际声音确认与家长录音替换占位旋律（[V2 验收](docs/hardware/V2_HARDWARE_TRIALS.md)）。
- Lucky DDNS 与外网连通验证、光猫端口转发（[S7](docs/SERVER_S7.md)）。
- 长稳、外壳装配、儿童安全红队（V7）。

## 开发阶段

板上实际有三向拨轮、QMI8658 姿态、PCF85063 时钟、SHTC3 温湿度、TG28 电源管理和 4 灰度电子纸（[完整配置](docs/hardware/BOARD_BASELINE.md#9-官方资料核对的完整硬件配置2026-11-02)）。

| 阶段 | 交付成果 | 状态 |
|---|---|---|
| V1 硬件能力验收 | 自检页 + 拨轮/传感器/时钟/电源/刷新实测 | 🔄 大部分已测，电源与刷新待复测 |
| V2 交互骨架 | 拨轮导航、首页动作条、选项语音播报、哄睡关机 | 🔄 代码与自动化试验完成，真机值守待做 |
| V3 作息与电源 | RTC 作息、定时开机问好、浅睡眠、低电量 | 🔄 代码已合并，3 天真机记录待做 |
| V4 体感、环境与内容 | 拿起/轻摇/倾斜、温湿度话术、灰度绘本、规则/体感游戏 | 🔄 代码已合并，整本绘本与误报真机测试待做 |
| V5 个性与记忆 | 三种性格、记忆、记忆驱动话术、服务端同步 | 🔄 代码已合并，真机与清空验收待做 |
| V6 语音与家长 | BOOT 说话联动、固定命令、家长面板 | 🔄 语音自动测试与开放问答进行中 |
| V7 稳定与安全 | OTA 回滚、长稳、外壳、红队 | ⬜ 未开始 |

方案文档：[产品需求](docs/PRODUCT_REQUIREMENTS.md) · [技术架构](docs/ARCHITECTURE.md) · [开发计划](docs/DEVELOPMENT_PLAN.md)（逐项状态）· [决策 ADR](docs/DECISIONS.md)。

## 文档导航

- [产品需求（PRD）](docs/PRODUCT_REQUIREMENTS.md)：年龄、功能边界、安全与 MVP。
- [技术架构](docs/ARCHITECTURE.md)：模块职责、控制流、显示与音频、数据一致性。
- [详细开发计划](docs/DEVELOPMENT_PLAN.md)：阶段、任务编号、依赖、验收、风险和交付物，以及逐项状态。
- [接口与数据模型](docs/INTERFACES_AND_DATA.md)：设备事件、业务 API、状态迁移、资源包规范。
- [服务端方案](docs/V5_V6_SERVER_PLAN.md) 与各阶段文档：`SERVER_S0`–`SERVER_S7`。
- [测试及上游更新策略](docs/TEST_AND_UPSTREAM.md)：硬件测试、升级兼容、性能测量和 OTA。
- [架构决策记录](docs/DECISIONS.md)：为什么选择自建服务、如何减少与小智版本耦合。
- 玩法与内容：[V8 养成](docs/V8_CARE_AND_GROWTH.md) · [V8 绘本](docs/V8_BOOKS.md) · [V8 小游戏](docs/V8_MINI_GAMES.md) · [V8 声音与作息](docs/V8_SOUND_AND_ROUTINES.md) · [学习内容流水线](docs/LEARNING_CONTENT_PIPELINE_QUICKSTART.md)。
- 语音：[V6 语音固定命令](docs/V6_VOICE_COMMANDS.md)。

## 仓库结构

```text
xiaozhi-pet/
├── README.md
├── CLAUDE.md                # 项目协作规则
├── docs/                    # 产品/设计/计划/协议/测试/硬件记录
├── firmware/                # 宠物应用组件与固件集成
├── services/pet_learning/   # 独立 Python 业务服务（API、配对、同步、面板）
├── deploy/                  # 服务端 compose、公网入口（ingress）等部署资料
├── content_pipeline/        # 学习内容导入与审核流水线
├── curriculum/              # 课程索引
├── hardware/                # 硬件与外壳 CAD
├── integrations/            # 上游集成
├── tools/ · scripts/        # 内容、自测与运维工具
├── tests/                   # 自动化测试
└── version-lock.json        # 上游版本锁定
```

## 上游与声明

- [ESP32 固件](https://github.com/78/xiaozhi-esp32)
- [Python 语音服务端](https://github.com/xinnan-tech/xiaozhi-esp32-server)
- [微雪开发板文档](https://docs.waveshare.net/ESP32-S3-ePaper-3.97/)

本仓库已有通过 CI 的 C++ 组件、服务端测试及真实 ESP32-S3 固件刷写/启动记录，但并不代表通过儿童产品安全审核、墨水屏体验或完整语音/硬件功能验收。儿童开放式 AI 对话须在家长参与下测试；上线前必须执行家长控制、隐私、输入/输出审核及失败降级验收。
