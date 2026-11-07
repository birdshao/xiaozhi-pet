# LCP-05：离线课程索引与跨内容包关联（COURSEINDEX1）

状态：本地只读目录 MVP。它**不**修改 ESP32 现有课程菜单，也不替代 V5/V6 服务端 S4 的内容审核和分发。

## 1. 为什么需要课程索引

KPB2 和 KPG1 是独立的二进制内容包，现有固件只会读绘本和 `/games/default.kpg`。要组织“认识图形 → 数数 → 大小比较 → 复习”的学习路径，需要在包装层之上维护稳定的技能 ID、课程单元和内容包引用，且必须核对实际字节不能只信任文件名。

本轮新增：
- `curriculum/skills.v1.json`：基础技能注册表（ID 稳定）。
- `curriculum/courses/shapes_and_numbers.v1.json`：三个单元，共用游戏包 `content_id=20001`。
- `content_pipeline/catalog.py`：严格校验目录元数据、所有课程引用与设备包的二进制完整性，输出机器可读索引。
- CLI `build-course-index`，单元测试与 CI。

## 2. 数据边界

COURSE1 是**工具侧课程编排**；COURSEINDEX1 是编译后、按具体 sha256 绑定的目录。索引不包含孩子的学习记录、录音、设备 ID、家庭信息。

`status` 永远不是 `approved`：
- `local_unverified`：只代表解析通过、sidecar 与文件 SHA-256 一致；它不是签名、审核授权或服务器的可信内容包。
- `test_only`：用户显式传 `--test-only`，可以包含 tone 测试游戏；**不能作为孩子可用的课程分发清单**。

每个游戏包仍需先经过 LCP-04 的人审流程，绘本同理；后续 S4 还必须自行验证审核和资源来源。

## 3. 生成本地目录

首先用上一阶段的 `make-math-game`、`stage-game`、`build-game` 制作经过家长审核的游戏包，放到：

    research_content/builds/games/default.kpg
    research_content/builds/games/default.manifest.json

在仓库根目录执行：

```bash
python -m content_pipeline.cli build-course-index \
  --course curriculum/courses/shapes_and_numbers.v1.json \
  --skills curriculum/skills.v1.json \
  --package research_content/builds/games/default.kpg \
  --output research_content/catalog/shapes_and_numbers.index.json
```

如果有实际批准的绘本，先在 COURSE1 的单元 `items` 中增加其 `kind=book`、`content_id`，再通过另一个 `--package books/<id>.kpb` 传入；引用了但没提供真实包必须拒绝。

**不会**自动下载、不写 TF 卡、不修改固件、不调用服务端 S4。

## 4. 索引结构

```json
{
  "schema_version": "COURSEINDEX1",
  "status": "local_unverified",
  "warning": "Not a device-install manifest, S4 approval, or a signature",
  "course_id": "preschool.shapes_and_numbers",
  "title": "形状与数量",
  "age_band": "3-5",
  "packages": [
    {
      "kind": "game",
      "content_id": 20001,
      "revision": 1,
      "title": "形状与数量",
      "sha256": "<actual KPG1 bytes sha256>",
      "bytes": 12345,
      "sd_path": "/games/default.kpg",
      "item_count": 15,
      "test_placeholder": false
    }
  ],
  "units": [
    {
      "unit_id": "find_shapes",
      "name": "认识基础形状",
      "skill_id": "math.shapes_basic",
      "items": [{
        "kind": "game", "content_id": 20001, "revision": 1,
        "sha256": "<actual KPG1 bytes sha256>",
        "sd_path": "/games/default.kpg"
      }]
    }
  ]
}
```

仅示意关键字段；实际生成含三个单元，sha256/bytes 必须从真实文件计算，不可手填示例。

## 5. CI 的 tone 测试方式

`tests/test_learning_content_catalog.py` 使用已有的合成 KPG1 和 KPB2 工具构造测试数据。tone 游戏 manifest 具有 `placeholder=true`，默认不能进入课程索引；只有测试明确传入 `allow_test_artifacts=True` 才能生成 `status=test_only` 索引。

```bash
python -m unittest discover -s tests -p 'test_learning_content_catalog.py' -v
```

已覆盖/计划覆盖：稳定 ID/技能检查、未知知识点、缺少包、重复包、读取真实二进制、manifest SHA 改变后拒绝、测试占位包隔离及 CLI 流程。

## 6. V2 设备端所需的后续工作

1. **按课程选包**：现有板端一次只加载 `/games/default.kpg`，必须增加安全的只读课程选择器、文件名白名单、内容包切换，才能真正选择多个主题。
2. **按技能选题**：KPG1 文件的二进制表中没有 skill 字段，现有 GameSession 从整个池子挑题。V2 要么让工具生成“每单元一包”，要么新增经过校验的题号映射（避免伪造答案或破坏现有格式）。
3. **学习进度分离**：孩子的练习记录应独立于 PetMemory；不应由服务端重放奖励，也不应依据一次答题正确与否评定儿童发展。
4. **S4 审核和下载**：本地索引不能代替整包 sha256 的服务器审批；KPB2/KPG1 内部 CRC 防损坏但不是签名。
