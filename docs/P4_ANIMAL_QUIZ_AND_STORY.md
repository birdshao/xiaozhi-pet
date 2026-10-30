# P4：3–5 岁儿童「猜动物」和原创短绘本 MVP

这是可运行的儿童学习业务，不涉及固件升级。

## 功能

- 4 道原创的两选一动物题（猫头鹰、大象、青蛙、小鸟），3 页原创短绘本《森林里的小伙伴》。
- 内容默认保存为草稿，只有受信任成人逐条审核并显式使用 --approved 才对设备可见。设备没有题库编辑/审批接口。
- Bearer Token 鉴权的游戏 API：POST /v1/devices/{id}/quiz/start、POST /quiz/answer、GET /quiz/status。
- 通过原有小智语音意图入口：说「猜动物」「再来一题」读出题目和选项；说「选一」「我选第一个」「选二」回答。
- 每设备独立 SQLite 进度，重启继续当前题目；重复说「猜动物」不会跳过题目。
- 答错可再试一次，连续两次没答对只作温和提示；不扣分、不惩罚、不操纵宠物成长奖励。
- 安静模式暂停游戏，取消审核的题目不会继续播报；未知语音保持固定兜底，不回显未经审核的 ASR。

## 本地操作（成人监督）

在仓库根目录，Python 3.10+：

    python -m pip install -r services/requirements.txt
    python -m tools.seed_sample_content --db data/pet_learning.sqlite3

以上只导入草稿。家长检查 services/pet_learning/sample_content.py 全部内容后，显式批准：

    python -m tools.seed_sample_content --db data/pet_learning.sqlite3 --approved

也可通过 tools/pet_learning_admin.py 的 import-quiz 命令导入独立题卡，默认依旧是草稿。

启动本机 API：

    python -m uvicorn services.pet_learning.api:app --host 127.0.0.1 --port 8765

设备凭据注册步骤见 services/README.md。语音入口 POST /v1/devices/{id}/intent 的固定词条返回 approved_content 或 fallback；现有小智儿童适配器只播报审核/固定文本。

## 当前边界

已实现可运行的 FastAPI/SQLite 逻辑及 Python 集成测试，但尚未在板卡麦克风、扬声器和墨水屏上完整验证，也没有将题库缓存到 ESP32。真实自建语音服务器仍需部署，网络断开时此在线学习服务不可用，已有离线宠物互动不受影响。

成人审批是必要步骤，不是自动内容安全认证。题目进度与设备 ID 是极少量业务数据；没有录音、原始 ASR 文本存档。家长管理页面、进度删除和离线学习资源包仍待实现。
