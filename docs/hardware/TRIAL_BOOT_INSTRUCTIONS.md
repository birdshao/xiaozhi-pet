# P1：OTA0 临时启动实验与自动切回 OTA1

状态：**OTA0 写入成功、已验证；新固件尚未启动验收**（2026-10-25）。

## 当前真实设备状态

- 已经成功写入 OTA0 的新应用镜像：小智 2.5.2 + PetDisplay，2,851,232 字节。
- [真实 OTA0 写入成功日志](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40195232072) 包含 `READBACK_OK`、`PRESERVATION_OK`、`STAGE_SUCCESS`。
- 写入后读回 SHA256 与发布镜像相同：`1ed92d996f4bb37f917ac9c31eecc7e4c9923fccc2d504711dc02a9ee204e69f`。
- 当前已确认的原启动选择记录仍指向 OTA1，小智旧版 2.2.38；OTA0 内原 2.2.37 已被测试固件替换。
- 完整 16 MiB 原 Flash 备份保存在 Windows Runner 本地，备份文件长度及 SHA-256 经多次验证通过；**不上传 GitHub**。

## 已准备的下一次手动工作流

[打开 Windows Runner 手动 OTA0 启动试验](https://github.com/smoothRobot/orpa-agent-runner/actions/workflows/xiaozhi-ota-trial.yml)

[实现 PR #17](https://github.com/smoothRobot/orpa-agent-runner/pull/17) · [软件逻辑测试通过](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40196442468)

**模式 1：`TRIAL_OTA0_AUTORESTORE_OTA1`**

1. 重新验证原始备份、当前 `otadata`、OTA1 镜像头以及已烧录的 OTA0 镜像 SHA256。
2. 通过一条 **4 KiB OTA 数据记录**临时把启动槽指向 OTA0；另一条原 OTA1 记录保持不动。如果这一条写入中断，原 OTA1 条目仍有效。
3. 设备复位并试运行约 **50 秒**。Runner 在这段时间收集**经过摘要化的串口统计**，不输出可能含 Wi-Fi 凭据的完整串口日志。
4. 无论试运行正常或 Python 捕获到普通异常，`finally` 尝试写回原始第一条 4 KiB OTA 数据记录，使 OTA1 再次成为启动目标。
5. 再读取完整 8 KiB `otadata`，确保与原备份逐字节一致，最终日志需要出现 **`RESTORE_SUCCESS`** 或 **`RESTORE_ALREADY_OK`**。

**模式 2：`RESTORE_ORIGINAL_OTA1`**

用于手动恢复已保留的 OTA1 启动记录。只读取本地原备份及设备 OTA 数据，再恢复原第一条选择记录；不依赖临时试运行是否成功、不从公网下载固件、不重写任何应用、NVS 或资源分区。即使普通自动恢复失败，仍有独立入口尝试救援。

### 如何启动

1. 在 GitHub 打开上面的工作流，选择 **Run workflow**，分支 `main`。
2. 下拉 `confirm` 的默认值是 **`NO_WRITE`**，此时不会执行写入。
3. **你准备好在设备旁观察时**，选择 **`TRIAL_OTA0_AUTORESTORE_OTA1`** 并启动。
4. 设备复位进入短暂试运行时，可观察屏幕是否呈现新版小智正常界面；如能进入空闲，**长按 BOOT** 尝试显示宠物，短按切换表情，长按退出（当前交互是实验代码，未实测）。
5. 结束后检查日志是否包含 `TRIAL_OTA0_SELECTED`、`SERIAL_SUMMARY`、`RESTORE_SUCCESS`。**只有后者验证成功才表示恢复到 OTA1 的目标已确认**。
6. 如检查失败、Runner 断电或 USB 失联，先停止其它固件实验；可单独选择 **`RESTORE_ORIGINAL_OTA1`** 尝试恢复，或根据完整本地备份制定人工恢复流程。

## 安全限制及剩余风险

- **与之前的 OTA0 固件写入不同，这次会短暂更改 `otadata`，并实际启动未验证的新应用。** 即使自动切回 OTA1，**2.5.2 应用仍可能在试运行阶段迁移或更改共享 NVS 配置**，因此不能承诺完全零影响。必要时要从本地敏感备份单独恢复配置，不能公开备份文件。
- 自动恢复依赖 Windows Runner、电脑供电、USB 串口和 Python 任务正常执行；**主机或硬件突然断电不能保证 `finally` 运行**。试运行前建议用户本人在设备旁。
- 如果新固件屏幕不刷新、打断功能异常或串口没有输出，也不应直接判定成功；此阶段核心目标只是检查能否启动和基本表情显示。
- 本工作流不上传敏感镜像；不对整片 Flash、`ota_0/ota_1` 应用镜像、NVS、assets 执行擦写，只对 `otadata` 的第一 4 KiB 执行切换与恢复。
- 自动切回的是**原启动选择记录**，并不自动还原新固件试运行时可能修改过的 NVS、外部资源、RTC 或云端设备状态。
- 如果你准备的是长期运行新版固件，不要继续使用 50 秒短时演练；应在真机基线测试和数据迁移验证后另行实施正式 OTA 升级。

## 下一次验收信息

需要记录以下非敏感信息：

- 是否看到新版小智启动界面？是否发生黑屏或循环重启？
- 长按 BOOT 后是否看到宠物脸部？短按能否切换、长按能否退出？
- 局刷残影、刷新速度、屏幕方向是否符合预期？
- 工作流是否结束并报告 `RESTORE_SUCCESS`，之后是否正常回到原小智界面？
- 语音/音频是否在新版试运行期间异常？

跟踪：[P1 Issue #2](https://github.com/birdshao/xiaozhi-pet/issues/2)。


## 2026-10-25：首次手动试运行失败与修复

- [失败运行 #40197657729](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40197657729)：PowerShell 抛出 `WhitespaceBeforeHereStringFooter`（内联 Python here-string 的闭合标记前出现空格），解析脚本时失败。
- 失败发生在 PowerShell 语法解析阶段，**没有进入 Python，未写入任何 Flash 或切换 OTA**；这次不能当作固件启动失败。
- [修复 PR #18](https://github.com/smoothRobot/orpa-agent-runner/pull/18) 已合并：用 Base64 字符串还原原 Python 脚本，避免嵌入源码被 YAML/heredoc 缩进破坏。
- [最新无硬件影响的验证运行 #40198134906](https://github.com/smoothRobot/orpa-agent-runner/actions/runs/40198134906) **通过**，包括解码后源码逐字节核对、`pwsh` 对生成 PowerShell 脚本做纯语法解析，以及 OTA 切换/中断/恢复单元测试。组织仓库常规 CI 同样通过。
- **旧失败运行不应点 Re-run jobs**，因旧 workflow 固定于出错的提交版本。请从该工作流页面创建一次全新的手动运行（`main`），并再次选择 `TRIAL_OTA0_AUTORESTORE_OTA1`。
- 目前 **OTA0 镜像已烧录成功，仍待真实启动验证**。即便语法问题已修复，试运行本身仍有 USB 失联、上游固件升级导致 NVS 变化等风险。请在设备旁观察、确认私密 Flash 备份与原 OTA1 恢复手段可用后再操作。
