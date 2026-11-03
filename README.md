# xiaozhi-pet — 儿童 AI 宠物与互动学习终端

> 项目状态：**纯官方固件真机基线已启动 / 宠物 MVP 开发中** · 文档基线：2026-10-25  
> 目标用户：**3–5 岁儿童（家长参与与控制）**  
> 硬件：**Waveshare ESP32-S3-ePaper-3.97**（800×480 墨水屏，三向拨轮，姿态/时钟/温湿度传感器，锂电管理，ESP32-S3）

## 项目定位

将“小智 AI”开源语音终端扩展为一只**能聊天、能互动、有持续状态、能陪孩子读绘本和学习**的墨水屏虚拟宠物。

不做“提示词 + 几张表情”的玩具；宠物行为、成长、画面、绘本进度、离线音效、家长控制由自己掌握，大模型负责适龄的语言能力，不决定底层关键操作。

### 技术路线（已选择）

1. **设备固件**：基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)，保留网络/音频/设备基础设施；增加隔离的 C++ 儿童应用组件。
2. **语音服务**：先验证 [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server) 自建 Python 服务端，ASR/LLM/TTS 采用可替换的本地或云端提供商。**自建不等于全部模型本地部署**。
3. **儿童业务服务**：独立 Python 服务，负责宠物长期状态、内容库、学习进度、家长设置和审计；语音服务通过边界明确的适配器调用。
4. **本地离线优先**：基础宠物交互和缓存故事不能依赖服务器在线；网络断开时降级，不丢失成长数据。
5. **维护策略**：固定上游版本、少量适配层、兼容性测试和自控 OTA。**官方固件升级不等于自建语音端每次都需升级**，只有协议或接口变化等触发适配。

**官方小智云的定位**：硬件/语音排障的参考基线，不是本项目的正式业务依赖；是否留作测试后备，依照之后实测决定。

## 开发阶段（v0.2，2026-11-02 按实物硬件重做）

板上实际有三向拨轮、QMI8658 姿态、PCF85063 时钟、SHTC3 温湿度、TG28 电源管理和 4 灰度电子纸（[完整配置](docs/hardware/BOARD_BASELINE.md#9-官方资料核对的完整硬件配置2026-11-02)）。方案据此重做：拨轮成为孩子的主操作件，宠物有作息和定时开机，加入体感、环境、灰度绘本和结构化的个性记忆。旧 P0–P4 代码保留复用。

| 阶段 | 交付成果 | 放行条件 |
|---|---|---|
| V1 硬件能力验收 | 自检页 + 拨轮/传感器/时钟/电源/刷新实测 | 每项硬件有真机结论 |
| V2 交互骨架 | 拨轮导航、新首页动作条、选项语音播报、PWR 哄睡关机 | 断网只用三个物理件完成首页全部入口 |
| V3 作息与电源 | RTC 作息、定时开机问好、浅睡眠、低电量 | 自己睡觉/起床，安静时段离线生效 |
| V4 体感、环境与内容 | 拿起/轻摇/倾斜、温湿度话术、灰度绘本 + 页内选择题、规则/体感游戏 | 整本绘本离线读完答题，静置无误报 |
| V5 个性与记忆 | 三种性格、今日/喜好/成长记忆、记忆驱动话术 | 记得昨天的事和孩子的选择，家长可清空 |
| V6 语音与家长 | BOOT 说话联动、固定命令扩展、家长入口 | 审核与降级用例通过 |
| V7 稳定与安全 | OTA 回滚、长稳、外壳、红队 | 才能考虑儿童日常使用 |

方案文档：[产品需求](docs/PRODUCT_REQUIREMENTS.md) · [技术架构](docs/ARCHITECTURE.md) · [开发计划](docs/DEVELOPMENT_PLAN.md)（含旧 P 阶段对照）· [决策 ADR-009~013](docs/DECISIONS.md)。

## 任务看板（GitHub Issues）

按里程碑依次推进，详细子任务及验收勾选在各 Issue 内：

- [#1 P0：硬件及上游基线](https://github.com/birdshao/xiaozhi-pet/issues/1)
- [#2 P1：宠物 UI 与墨水屏刷新验证](https://github.com/birdshao/xiaozhi-pet/issues/2)
- [#3 P2：离线宠物 MVP](https://github.com/birdshao/xiaozhi-pet/issues/3)
- [#4 P3：自建 AI 语音与宠物联动](https://github.com/birdshao/xiaozhi-pet/issues/4)
- [#5 P4：绘本、学习与养成](https://github.com/birdshao/xiaozhi-pet/issues/5)
- [#6 P5：家长管理、OTA、安全与长期维护](https://github.com/birdshao/xiaozhi-pet/issues/6)

## 当前实施进度（2026-10-25）

### 纯官方固件：真实硬件已刷入并启动

- [纯官方小智 v2.5.2 固件包](https://github.com/birdshao/xiaozhi-pet/releases/tag/official-waveshare397-v2.5.2-20261025) 直接由未修改的上游 `78/xiaozhi-esp32` 构建；应用与匹配资源 SHA 均已锁定。
- [Windows Self-hosted Runner 真机安装成功 #40203387106](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40203387106)：OTA0 写入官方 app-only 与匹配资源、读回 hash、选中 OTA0；串口确认版本 2.5.2 启动，未观察到匹配的 Panic/WDT 标记。
- OTA1 保留用户原来的 2.2.38 应用；**这不能称为官方原版**。原 16MB Flash 完整备份仅保存在 Windows Runner 本地。原 bootloader、partition table、NVS 没有被该烧录程序直接改写；共享 assets 已更新，回退旧应用时需检查资源兼容性。
- [详细实测与准确 Flash 范围](docs/hardware/OFFICIAL_BASELINE_20261025.md)。**屏幕、麦克风、喇叭、Wi-Fi 和 SD 尚待实机功能验收**。
- 一次性物理烧录 PR [#21](https://github.com/smoothRobot/orpa-agent-runner/pull/21) 已关闭、未合并到组织主分支，避免重复触发。

### 宠物功能：独立迭代中

- [宠物黑白渲染与 PetDisplay 集成 PR #9](https://github.com/birdshao/xiaozhi-pet/pull/9) 已编译；[实验用 app-only 固件包](https://github.com/birdshao/xiaozhi-pet/releases/tag/p1-device-preview-20261025) 保留在 GitHub，**但不在当前启动的官方固件里**。
- [P2 C++ 宠物状态机 PR #12](https://github.com/birdshao/xiaozhi-pet/pull/12) 已合并、主机与固件构建通过；支持喂食、故事、语音状态、安静时段及有限 RAM 去重。
- P2 定长事件队列仍在测试：见 [PR #13](https://github.com/birdshao/xiaozhi-pet/pull/13)。
- [P2 实现边界说明](docs/P2_PET_ENGINE.md)；未完成设备端事件任务、跨重启存档、真实画面联动及音频协调。

## 机械外壳 V5（2026-10-25）

- [猫咪儿童外壳 CAD 源码与设计说明](hardware/enclosure/cat-v5/README.md)
- [前后壳 3MF / STL / STEP / BREP 实体文件](hardware/enclosure/cat-v5/output/)
- [自动 CAD 构建工作流](.github/workflows/enclosure-cad-v5.yml)

此版本重建了猫头外形与隐藏固定结构，保留原始屏幕及三处独立侧壁功能开口；扬声器音栅按 20 × 30 mm 设计。通过 CAD/网格检查，**尚未完成实体装配和儿童安全测试**。

## 当前基线：纯官方 XiaoZhi 已在真机 OTA0 启动（2026-10-25）

- [真实官方安装运行](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40203387106) 验证**未修改上游 v2.5.2 应用**（2,848,304B）和相应官方 assets（2,900,513B）均已写入/读回成功，OTA0 启动确认，串口监测 `panic=False watchdog=False`；旧 OTA1 程序保留。
- 设备原有 OTA1 的 2.2.38 **不是用户要求的官方纯净基线**，只是恢复备份。官方基线已单独安装，不需重复刷入。
- [官方源码可复现构建 #14](https://github.com/birdshao/xiaozhi-pet/pull/14) 已合并；[构建 CI](https://github.com/birdshao/xiaozhi-pet/actions/runs/40204407387) 成功。
- 为避免重复写入，另一套 [Runner 刷写 PR #22](https://github.com/smoothRobot/orpa-agent-runner/pull/22) 已关闭、未执行。
- **下一阶段**：官方基线屏幕/声音/输入进一步真机验收；儿童应用源码独立开发，不直接改写当前官方 OTA0。
- [P2 宠物状态→画面桥接 #15](https://github.com/birdshao/xiaozhi-pet/pull/15) 已合并；C++ 自动测试通过。

## P2：离线宠物核心持续开发（2026-10-25）

- [PR #16 双槽 NVS checkpoint](https://github.com/birdshao/xiaozhi-pet/pull/16) 已合并，支持 1064B 版本化 CRC 存档、最近 128 条事件跨重启去重、提交失败时不发奖。
- [PR #17 FreeRTOS 设备端事件任务](https://github.com/birdshao/xiaozhi-pet/pull/17) 已合并，[完整 ESP-IDF 固件构建](https://github.com/birdshao/xiaozhi-pet/actions/runs/40208607677) 通过；本地动作 ID 从持久代号导出，队列满时拒绝、NVS 故障时队头重试。[设备事件说明](docs/P2_DEVICE_EVENT_SERVICE.md)。
- [PR #18 AudioCoordinator](https://github.com/birdshao/xiaozhi-pet/pull/18) 已合并，语音优先/故事恢复/宠物音效/安静时段的纯策略测试通过。[音频设计](docs/P2_AUDIO.md)；[ESP-IDF 组件集成 PR #19](https://github.com/birdshao/xiaozhi-pet/pull/19) 正在验收。
- 以上是**代码/CI 交付**，尚未完成真实按钮、扬声器、NVS 断电恢复的物理联调。已安装的官方 OTA0 固件保持不变。

## P1/P2 新增：电子纸画面邮箱与刷新节流

- [PR #20 已合并](https://github.com/birdshao/xiaozhi-pet/pull/20)：`FreertosPetSceneMailbox` 只在存档成功后接收宠物场景，覆盖队列只保留最新画面；`PetRefreshGate` 默认最短 5s 全刷完成间隔；`FreertosPetDisplayWorker` 在专用任务中调用物理渲染回调，刷新失败不提前提交画面状态。
- `scripts/p1_integrate.py` 新增微雪 `CustomEpdDisplay::ShowPetScene(SceneModel)`，原 BOOT 表情预览行为保持兼容。
- [刷新设计与尚缺的真机接线步骤](docs/P1_REFRESH_GATE.md)。上述通过主机测试及组件编译验证，**还没有在实体设备启用这条宠物显示链路**；纯官方 OTA0 未刷写。

## BIRDSHAO 设备联调与 Pet 专用板卡入口

- 用户已将微雪 ESP32-S3-ePaper-3.97 板卡连接至新的 **BIRDSHAO** Windows Runner。旧 `DESKTOP-QMOCC9V` / COM10 记录不可直接复用。
- [组织 Runner 只读 USB 检测工作流](https://github.com/smoothRobot/orpa-agent-runner/actions/workflows/xiaozhi-birdshao-readonly.yml) 已部署，[安全 CI](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40251421525) 通过；需用户从 GitHub 手动执行并先验证真实 Runner 名及 USB/COM。
- [PR #21 板卡入口已合并](https://github.com/birdshao/xiaozhi-pet/pull/21)：待机时 BOOT 长按进入/退出宠物模式；单击互动/绘本翻页、双击故事、三击喂食；P2 NVS 事务及独立 EPD 刷新任务接入专用 Pet 固件。**未对实体板卡刷写**。
- [完整 IDF6.1 板卡编译通过](https://github.com/birdshao/xiaozhi-pet/actions/runs/40252096543)：测试 App 2,857,408 B，单 OTA 槽 4,128,768 B。
- [BIRDSHAO 新 Runner + OTA 测试步骤](docs/hardware/BIRDSHAO_RUNNER.md)。确认端口、备份、分区/官方 OTA0 状态和可恢复性之前，不进行 OTA1 物理写入。原纯官方 OTA0 仍保留。

## 真机首次启动 Pet 固件（2026-11-02）

- OTA1 升级为 main 固件 [build #41076469802](https://github.com/birdshao/xiaozhi-pet/actions/runs/41076469802)（4b18663，SHA `b3760e14…`），读回校验通过。
- 有人值守的 60 秒试验：`Running partition: ota_1`、被动启动标记、长按 BOOT 后 `PET_RUNTIME_ON`，无 panic/WDT；随后 OTADATA 精确恢复，官方 OTA0 重新启动。
- 画面质量、点击事件、SD 绘本音频、语音和回滚能力仍待验证。[详细记录](docs/hardware/OTA1_BOOT_TRIAL_2026-11-02.md)。

## 文档导航

- [产品需求（PRD）](docs/PRODUCT_REQUIREMENTS.md)：年龄、功能边界、安全与 MVP。
- [技术架构](docs/ARCHITECTURE.md)：模块职责、控制流、显示与音频、数据一致性。
- [详细开发计划](docs/DEVELOPMENT_PLAN.md)：阶段、任务编号、依赖、验收、风险和交付物。
- [接口与数据模型](docs/INTERFACES_AND_DATA.md)：设备事件、业务 API、状态迁移、资源包规范草案。
- [测试及上游更新策略](docs/TEST_AND_UPSTREAM.md)：硬件测试、升级兼容、性能测量和 OTA。
- [架构决策记录](docs/DECISIONS.md)：为什么选择自建服务、如何减少与小智版本耦合。

## 预计仓库结构（规划，不表示代码已实现）

```text
xiaozhi-pet/
├── README.md
├── docs/                    # 产品/设计/计划/协议/测试
├── firmware/
│   ├── components/kid_app/  # PetController, PetDisplay, Inputs, Audio, Storage
│   └── integration/         # 与小智官方固件的最薄适配
├── services/
│   ├── pet-learning/        # 独立 Python 业务服务
│   └── xiaozhi-adapter/     # 自建小智语音服务端对接
├── content/                 # 绘本/图片/声音的源文件和生成工具
└── tests/                   # 状态机、契约、音频/墨水屏真机回归
```

**注意**：目录是拟议布局，P0 后确认是否以 Git submodule、外部 Checkout 或构建补丁的方式接入上游，不宜提前直接复制整份小智源码。

## 第一项工作

按照 [V1 硬件能力验收](docs/DEVELOPMENT_PLAN.md#v1硬件能力验收) 在真实开发板上逐项确认：拨轮三向与 GPIO4/5/6 的对应、I²C 设备应答、姿态手势数据、时钟闹钟定时开机、电量读数、局刷/灰度刷新耗时与残影、浅睡眠唤醒。资料已核对不等于固件已接入或真机可用。

## 上游与声明

- [ESP32 固件](https://github.com/78/xiaozhi-esp32)
- [Python 语音服务端](https://github.com/xinnan-tech/xiaozhi-esp32-server)
- [微雪开发板文档](https://docs.waveshare.net/ESP32-S3-ePaper-3.97/)

本仓库已有通过 CI 的 C++ 组件及真实 ESP32-S3 固件刷写/启动记录，但并不代表通过儿童产品安全审核、墨水屏体验或完整语音/硬件功能验收。儿童开放式 AI 对话须在家长参与下测试；上线前必须执行家长控制、隐私、输入/输出审核及失败降级验收。
