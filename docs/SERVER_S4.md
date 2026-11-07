# 服务端 S4：内容目录与自动分发

对应 `V5_V6_SERVER_PLAN.md` §7.4、§6.3、§9、§11.1。代码：`services/pet_learning/content.py`（导入、审核、派生）、`distribution.py`（分配、manifest、Range 下载、回报、清单比对、路由）、迁移 `003_content.sql`、参考下载器 `tools/fake_device.py`。测试：`tests/test_v6_content_catalog.py`。

## 接口

| 路由 | 鉴权 | 说明 |
|---|---|---|
| `POST /v1/parent/content[?pack_id=]` | P+CSRF | 原始字节上传（`application/octet-stream`，≤64 MiB）。按魔数分派 `tools/kpb2.py`/`game_pack.py`/`sound_pack.py` 的现有校验器；失败 415/422。存 `packages/<sha256>`，状态 `draft`；同字节重复上传返回已有包。`pack_id` 只用于游戏/声音包的卡上文件名 |
| `GET /v1/parent/content` | P | 目录 |
| `POST /v1/parent/content/{id}/approve` | P+CSRF | `{"sha256", "checklist":{项:true,…}}`；sha256 必须等于该包，全部勾选；审核时重读并重算文件哈希。同路径旧 revision 自动 `retired`，已分配旧版本的设备自动改装新版；新 revision 必须比已审核的大（`REVISION_NOT_NEWER`） |
| `POST /v1/parent/content/{id}/retire` | P+CSRF | 已装的包转为"待删除"，未装的分配直接撤销 |
| `GET/PUT /v1/parent/devices/{id}/content` | P(+CSRF) | `PUT {"items":[{"package_id","wanted":"install"\|"remove"}]}`；只能安装已审核包；`remove` 只作用于已跟踪的分配（手工拷的文件永远不动）。GET 返回分配状态、`NO_SPACE`、卡上清单（`approved`/`outdated`/`unreviewed`）、`sd_free_kib` |
| `GET /v1/devices/{id}/content/manifest` | D | `{install:[{kind,content_id,revision,sha256,header_crc,bytes,path,url}], remove:[…], withheld:[{sha256,reason:"NO_SPACE"}]}` |
| `GET\|HEAD /v1/content/packages/{sha256}` | D + `X-Device-Id` | 仅已审核且分配给该设备的包，否则 404（与不存在无异）。`Range`（单段、`a-b`/`a-`/`-n`）→ 206；不可满足 → 416 + `Content-Range: bytes */size`；语法错误/多段/`If-Range` 不符 → 忽略 Range 返回 200。`ETag: "<sha256>"`，固定 `Content-Disposition`，64 KiB 分块流式读文件。每设备同时 1 个下载（429 `DOWNLOAD_BUSY`，`Retry-After: 5`；30 s 无数据的租约视为过期） |
| `POST /v1/devices/{id}/content/report` | D | `{"sha256","state":"downloading\|installed\|failed\|removed","reason":"SHA256_MISMATCH","attempts":n}`；`failed` 必须带 reason |
| `PUT /v1/devices/{id}/content/inventory` | D | `{"items":[{kind,content_id,revision,header_crc,bytes}],"sd_free_kib":n}`：把卡上清单和分配比对（装好的标 installed、卡上没了的回到 pending、已删的清掉）。S2 的快照处理可直接调 `Distribution.reconcile(device_id, items, sd_free_kib)` |

卡上路径：书 `/books/c<content_id>.kpb`；游戏 `/games/<pack_id 或 g<content_id>>.kpg`；声音包 `/sounds/<pack_id，默认 default>.kps`。**路径是同一"槽位"的身份**：同路径只有一个 approved。

## 给其他模块的钩子

* **页问答（S5）**：`app.state.content_catalog.page_text(content_id, revision, page, header_crc=None) -> str | None`。只返回 approved 书的审核页文本；`header_crc`（设备在 `PUT context` 里报的包头 CRC，u32）不符返回 `None`。`approved_by_fingerprint(kind, revision, header_crc, content_id=None)` 给 KPS1（无 content_id，按 revision + CRC）。喜好/选项名：`like_names()`。
* **`SYNC_CONTENT`（S2）**：`catalog.on_sync_needed(lambda device_id: ...)`，在分配变更、新 revision 自动升级、撤销后（提交之后）对每台受影响设备调用；回调抛错不会回滚。S2 合并前设备按快照频率拉 manifest。
* **空间**：manifest 按最近上报的 `sd_free_kib` 扣除 1 MiB 保留后贪心派发，放不下的进 `withheld`。未上报时不限制。

## 迁移编号

本分支用 `003_content.sql`（基于 main）。S1 分支也有 `003_pairing.sql`，后合并的一方需把自己的迁移改成下一个编号（文件内容不依赖顺序，只依赖 `devices` 与 `audit_log`）。`test_v5_server_migrations` 改为按迁移文件数断言版本号。

## 部署注意

`content.py` 把仓库根加入 `sys.path` 以导入 `tools/` 的解析器，所以 S7 的镜像必须带 `tools/`（含 `kpb2.py`、`game_pack.py`、`sound_pack.py`）。包目录默认 `<数据库目录>/packages`，可用 `KID_PET_PACKAGES_DIR` 覆盖。导入的本机入口：`python -m services.pet_learning.content import-pack FILE [--pack-id ID]`（S7 可把它接进 `pet_learning_admin`）。

## 与离线流水线（#80–#90）的衔接

导入只依赖包本身的校验器，不读 `content_pipeline`；流水线产出的"已审核包 + manifest"可直接上传为 draft，再在面板按其 sha256 审核（manifest 里的 `sha256`/`checked_hash` 应等于这里的包 sha256，将来可在导入时比对）。
