# xiaozhi-pet — 儿童 AI 宠物与互动学习终端

> 项目状态：**架构设计 / 待硬件验证** · 文档基线：2026-10-25  
> 目标用户：**3–5 岁儿童（家长参与与控制）**  
> 硬件：**Waveshare ESP32-S3-ePaper-3.97**（800×480 黑白墨水屏，ESP32-S3）

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

## 开发阶段

| 阶段 | 交付成果 | 放行条件 |
|---|---|---|
| P0 硬件基线 | 小智固件复现与硬件测试记录 | 语音、屏幕、输入、电源、网络、SD 逐项确认 |
| P1 宠物画面技术验证 | 自定义全屏界面 + 表情切换 + 刷新调度 | 真机刷新、残影、内存、响应可接受 |
| P2 离线宠物 MVP | 本地状态机、输入/IMU、离线音效、状态存储 | 断网可玩，重启恢复，不阻塞语音 |
| P3 自建 AI 对话 | 自建 Python 语音服务、适龄对话、安全拦截、状态联动 | 规定场景可复现，ASR/TTS 延迟与安全测试通过 |
| P4 故事与学习 | 资源包、离线播放器、绘本、小游戏、学习记录 | 家长审查过的内容可离线完整播放 |
| P5 稳定性与维护 | 家长管理、OTA、兼容性 CI、真机回归 | 恢复升级、断网重连、数据迁移验证通过 |

完整拆解、优先级、任务依赖与验收标准请看 [开发计划](docs/DEVELOPMENT_PLAN.md)。

## 任务看板（GitHub Issues）

按里程碑依次推进，详细子任务及验收勾选在各 Issue 内：

- [#1 P0：硬件及上游基线](https://github.com/birdshao/xiaozhi-pet/issues/1)
- [#2 P1：宠物 UI 与墨水屏刷新验证](https://github.com/birdshao/xiaozhi-pet/issues/2)
- [#3 P2：离线宠物 MVP](https://github.com/birdshao/xiaozhi-pet/issues/3)
- [#4 P3：自建 AI 语音与宠物联动](https://github.com/birdshao/xiaozhi-pet/issues/4)
- [#5 P4：绘本、学习与养成](https://github.com/birdshao/xiaozhi-pet/issues/5)
- [#6 P5：家长管理、OTA、安全与长期维护](https://github.com/birdshao/xiaozhi-pet/issues/6)

## 当前实施进度（P0 + P1 主机原型）

- **已合并**：[P0 固件工程基线 #7](https://github.com/birdshao/xiaozhi-pet/pull/7)；[P1 场景原型 #8](https://github.com/birdshao/xiaozhi-pet/pull/8)。
- **可运行的代码**：固定上游 Commit、拉取脚本、板卡配置检查、host 测试和 GitHub Actions 原版固件构建。
- **硬件记录**：[P0 板卡基线与实物待测清单](docs/hardware/BOARD_BASELINE.md)。
- **验证状态**：[P0 固件 CI](https://github.com/birdshao/xiaozhi-pet/actions/runs/40187425730) 和 [P1 C++ 主机测试](https://github.com/birdshao/xiaozhi-pet/actions/runs/40187855638) 均通过。**实物烧录/墨水屏测试尚未完成**；不将 CI 成功当作真机通过。
- **P1 原型**：[独立黑白宠物场景设计](docs/P1_SCENE_PROTOTYPE.md)，可在本机执行 `bash scripts/p1_preview.sh` 生成 PBM 预览。

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

按照 [P0 计划](docs/DEVELOPMENT_PLAN.md#p0硬件及上游基线) 用真实开发板复现官方固件，特别确认：BOOT/PWR 和所谓“拨轮”是否在实际购入版本上存在、屏幕刷新策略、SD 读取、IMU 引脚与 OTA 分区。不要把产品宣传能力视为固件已接入功能。

## 上游与声明

- [ESP32 固件](https://github.com/78/xiaozhi-esp32)
- [Python 语音服务端](https://github.com/xinnan-tech/xiaozhi-esp32-server)
- [微雪开发板文档](https://docs.waveshare.net/ESP32-S3-ePaper-3.97/)

本仓库目前为设计文档，不代表已有经过安全审核的儿童成品、完成固件编译或通过真机测试。儿童开放式 AI 对话须在家长参与下测试；上线前必须执行家长控制、隐私、输入/输出审核及失败降级验收。
