# P4 连续绘本讲述（自建小智语音服务）

新增 StoryReader 作为独立的、基于已审核页面的阅读进度器。无需修改现有 ESP32 本地宠物动作协议。

## 使用流程

家长先用 tools/seed_sample_content.py 把原创短绘本导入为草稿，人工检查后再批准发布。

孩子可以说：
- “给我讲故事”或“讲故事”：从第 1 页开始，若上次没结束则继续当前页
- “下一页”或“继续故事”：播报下一张已审核的页面
- “再读一遍”：重读当前页
- “结束故事”：停止当前书

全部语音文字来自已审核的页面或固定的适龄提示；无任意 LLM 生成、无原始儿童录音或 ASR 文本存储。每台设备拥有独立的阅读状态，SQLite 重启后可继续当前页。一本书读完后可以再次开始；多本书会按 ID 确定性轮换。

## API

所有路径都需与原 PetLearning 服务一致的设备 Bearer Token：

- POST /v1/devices/{device_id}/story/start
- POST /v1/devices/{device_id}/story/next
- POST /v1/devices/{device_id}/story/repeat
- POST /v1/devices/{device_id}/story/stop
- GET /v1/devices/{device_id}/story/status

自建小智适配器调用现有 POST /v1/devices/{device_id}/intent 即可，通过预设语音词条分发上述操作。为了兼容原先通过 PET 事件进入 STORY 模式的设备，旧有 Story 事件的 NEXT/STOP/READ 语音行为优先保留。

仅第一页通过审核的绘本允许开始；每次翻页、重读都会重新检查当前页面仍是批准状态，撤销审核立即停止对该页的播报。每次返回 TTS 最长 320 个字符，符合已实现的小智儿童语音守卫的限制。

## 尚未完成

这是可运行的语音业务后端，但还未真正连接开发板的扬声器与墨水屏，也没有完成离线故事资源包、语音 TTS 音色、音频播放暂停/抢占或板卡端页面缓存。正在使用的 ESP32 原有宠物固件不受此次提交影响。设备仍需通过自建小智 Python 语音服务器与这个业务 API 通信；不能把 Python 集成测试当作实体板卡测试。
