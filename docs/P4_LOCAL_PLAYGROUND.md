# P4 本机交互体验台：宠物、绘本、猜动物

此功能将已实现的 FastAPI 业务接口连成一个可以在普通电脑浏览器里实际点击和试听的儿童互动原型。**它不是 ESP32 真机验收**，不要求先部署小智语音服务器，也不处理 OTA。

## 在本机启动

1. 从仓库根目录安装 Python 3.10+：`python -m pip install -r services/requirements.txt`。
2. 生成一个专门用于浏览器的、至少 32 位的随机测试令牌，设置环境变量 `KID_PET_DEVICE_TOKEN`。**不要与真实 ESP32 设备共用 ID 或令牌。**
3. 本地登记测试设备：`python -m tools.pet_learning_admin enroll --device-id pet-demo-001`。
4. 导入原创学习内容为草稿：`python -m tools.seed_sample_content`。请先亲自审核 `services/pet_learning/sample_content.py`，再显式批准：`python -m tools.seed_sample_content --approved`。
5. 设置 `KID_PET_ENABLE_PLAYGROUND=1`，启动 `python -m uvicorn services.pet_learning.api:app --host 127.0.0.1 --port 8765`。
6. 在**同一台电脑**打开 http://127.0.0.1:8765/play，输入新建测试设备 ID 和设备令牌，进入页面。
7. 点击宠物、绘本、猜动物三个标签；可以切换页面的朗读按钮。浏览器 `speechSynthesis` 的实际中文音色与平台有关，朗读可能依赖系统或浏览器供应商的服务，测试时只播报已经审核的文字或固定提示。

## 本次开发实现

- 大尺寸触控按钮、三个可切换界面、响应式适配与系统“减少动态效果”偏好；无远端字体、JS 框架或第三方图片。
- 喂食直接发送符合 Python 事件协议的 `PET_FEED`，校验 `message_id` 和 `seq` 后再显示成功与更新互动次数。网络故障时在**当前页面会话**保留同一事件待重试，避免把未确认结果说成成功；刷新网页后不保证恢复待确认事件，所以该演示**不是断电可靠队列**。
- “打招呼”仅产生本地视觉反馈，不假装修改设备端的宠物成长状态。
- 绘本：开始、下一页、重读、结束；只显示当前服务端返回的家长审核文本。
- 猜动物：开始、选一、选二；只播报规则引擎返回的固定/审核文字，不让 LLM 自行评分。
- 设备 Token 仅存在当前标签页的 JS 内存里；不会放到 URL、Cookie、localStorage 或 sessionStorage。退出测试会清空内存中的令牌。所有用户数据用 `textContent` 插入，不解析为 HTML。
- 开发页面默认关闭（`KID_PET_ENABLE_PLAYGROUND` 未设置），启用后仅推荐 uvicorn 绑定 127.0.0.1 使用；静态文件设置 CSP、no-store、nosniff 和禁止摄像头/麦克风权限。
- 不增加新的发布、审核、家长管理 HTTP 路由。内容审核仍必须通过本地成人操作。

## 检验

运行：
`python -m unittest discover -s tests -p 'test_p4_local_playground.py' -v`

CI 同时检查 `node --check services/pet_learning/playground/play.js`，在 Python 3.10 和 3.12 上执行现有全部故事、小游戏、语音适配和本次页面集成测试。

## 下一步真正的板卡闭环

目前 Waveshare Pet 模式仍由 BOOT 按键及旧版 `FreertosPetService` 驱动，独立 HTTP 同步模块尚未切换启用；其墨水屏不显示本 HTML。下一步应将场景选择/故事页码/游戏两选一抽象为与 GUI 无关的 `LearningInput` → `LearningState` 协议，让 BOOT/外部控制（如果实物支持）对应到同一套故事与问答行为；实际中文字形、音频资源与按键并行仍需在板卡上验证。

禁止把浏览器上的功能成功当作 ESP32 实物网络连接、刷屏、扬声器或儿童使用安全验收。
