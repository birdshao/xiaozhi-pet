# P4 离线绘本包 KPB1：审核、生成、SD 按页读取

本功能把家长审核的 800×480 黑白书页 PNG + 24 kHz 单声道 WAV 音频编译成一个可复制到 SD 卡的 KPB1 文件。运行时不要求连接 Python API 或云服务。该格式与现有 ESP32-S3 的 1-bit 画面约定一致（bit=1 白色）。

## 交付代码

- tools/story_pack.py：render 根据授权字体生成中文正文 PNG；pack 组合审核过的 PNG/WAV/文字，生成 .kpb 与 .manifest.json。
- firmware/components/kid_app/include/kid_app/story_pack_reader.h、src/story_pack_reader.cc：纯 C++17，有限栈空间，逐个 CRC 校验，支持 48,000 字节画面读取、16-bit PCM 按需分块读取。
- story_pack_file_port.h/.cc：基于已经挂载的 ESP-IDF VFS/FAT 的 fopen/fread，只读访问，不猜测 SD 引脚或挂载参数。
- tests/test_p4_story_pack.py 与 tests/cpp/test_story_pack_reader.cc：从同一个 Python 输出包验证真实 C++ 打开、读取、CRC/截断/非法路径拒绝。
- GitHub Actions p4-story-pack.yml：Python 3.10/3.12 + g++ C++17，不连接硬件。

## 家长本地资源目录

    my_book/
      book.json
      page_01.png
      page_01.wav
      page_02.png
      page_02.wav

book.json 示例：

    {
      "schema_version": 1,
      "book_id": "forest_friends",
      "title": "森林里的小伙伴",
      "approved": true,
      "pages": [
        {
          "number": 1,
          "text": "早晨，小兔子来到森林里。树叶轻轻摇晃。",
          "approved": true,
          "image": "page_01.png",
          "audio": "page_01.wav"
        },
        {
          "number": 2,
          "text": "小兔子遇见小鸟。小鸟说：你好呀！",
          "approved": true,
          "image": "page_02.png",
          "audio": "page_02.wav"
        }
      ]
    }

approved:true 是本地可信成人实际审查文字、画面及音频内容的声明，不是自动审查。pack 命令额外必须使用 --reviewed，且没有面向设备的审批 HTTP API；不能将标记视为签名或防篡改机制。

## 制作流程

在仓库根目录安装依赖：

    python -m pip install -r tools/story_pack_requirements.txt

可以用自己的插画软件制作严格 800×480 PNG；也可以使用本工具将中文标题和正文渲染成黑白书页，字体必须自行取得合法许可：

    python -m tools.story_pack render --font /path/to/authorized-CJK-font.ttf --title "森林里的小伙伴" --text "早晨，小兔子来到森林里。树叶轻轻摇晃。" --page 1 --output my_book/page_01.png

图像统一按亮度阈值 170 转换为 1-bit。建议成人实测细线、字号和颜色变黑后的画面清晰度。音频要求无压缩 WAV、24kHz、单声道、PCM16、每页最多 60 秒。请亲自听审音频，不要将合成的 CI 测试音作为给孩子的读物。

完成逐页审核后：

    python -m tools.story_pack pack --source my_book/book.json --output my_book/forest_friends.kpb --reviewed

导出两个文件：forest_friends.kpb（SD 内容），forest_friends.manifest.json（SHA-256、每页图片和声音摘要、审核记录、时长）。建议 SD 目标位置为 /sdcard/books/forest_friends.kpb，但必须先由板卡验证并挂载 SD 才能使用。

## KPB1 固定小端协议

头部 32B：struct 格式 <4sHHHHIIIII。

- 0–3：魔数 KPB1
- 4：uint16 版本 1；6/8：uint16 宽高 800/480
- 10：uint16 页数 1–32
- 12/16：uint32 TOC 起始偏移 32 和 TOC 字节数（页数乘 64）
- 20/24：uint32 数据起始偏移和包总长度，不超过 64 MiB
- 28：uint32 前 28 字节 CRC32

每页 TOC 固定 64B，struct 格式 <HHIIIIIIIHHIII16s，记录页码、保留字段、图片偏移/长度/CRC、PCM 偏移/长度/CRC、音频采样率/声道/bitdepth、UTF-8 正文偏移/长度/CRC 和 16B 全 0 保留字段。各数据段按 4B 对齐。图片恰为 48,000B，音频在包内储存为去掉 WAV RIFF 头的 little-endian signed PCM16。读取器逐段 CRC 检查后才报告打开成功，单次验证仅使用 512B 栈缓冲。调用方须提供 PSRAM 48KiB 书页帧缓冲；音频按段读取。

注意 CRC32 只能检测非恶意数据损坏，不证明内容真实性或版权。面向不可信 SD 或网络同步时必须增加独立签名验证与家长白名单。

## 本轮功能边界

读取器已经编入 kid_app 组件，可在 ESP-IDF FAT VFS **已完成 SD 挂载**的前提下打开包。但目前 Waveshare 固件未验证实际 SD 针脚/总线和挂载，没有将这个包的图片交给 SceneComposer/EPD，也没有将 PCM 交给扬声器驱动。软件可读取 PCM 并不等于实体设备能播放故事。下一阶段必须在板卡上验证挂载与总线，再由显示任务和 AudioCoordinator 接管书页与音频；断网阅读的硬件验收尚未完成。
