# P1: 纯 C++ 宠物场景原型（非板载 UI）

状态：2026-10-25 实现草案。此阶段的“可运行”指主机端编译与执行，不是 ESP32 真机显示。

## 为什么先做这个？

把“宠物目前是什么场景、什么表情”与“墨水屏怎样通过 SPI 刷新”拆开，避免以后小智 Display/LVGL 更新造成整个 Pet Engine 重写。这个原型输出与电子纸相同尺寸的 1bpp 黑白帧，不依赖官方小智头文件。

## 当前文件

- [scene_model.h](../firmware/components/kid_app/include/kid_app/scene_model.h)：`SceneKind`、`Emotion`、`SceneModel`、`MonoFrame`。
- [scene_model.cc](../firmware/components/kid_app/src/scene_model.cc)：有限个姿态的确定性绘图，黑白 800×480。
- [pet_scene_preview.cc](../tools/pet_scene_preview.cc)：生成 `P4 PBM` 图片，预览模型输入产生的实际像素。
- [test_pet_scene.cc](../tests/cpp/test_pet_scene.cc)：缓冲区尺寸、边界保护、表情区别、绘图确定性。

### 在开发机上预览

```bash
bash scripts/p1_preview.sh
```

输出 `build/p1/{happy,sleepy,curious,reader}.pbm`。PBM 可以用 GIMP、ImageMagick 等打开。全部为黑白线稿占位图，不是最终美术素材。

## 已覆盖 / 未覆盖

**实现意图**：
- `SceneModel` 不引用小智、LVGL 或设备驱动；
- 1bpp 48,000 字节缓冲区；bit=1 是白色，bit=0 是黑色（和小智当前 `EPD_DrawColorPixel` 的缓冲约定匹配）；
- 表情绘制、阅读场景、书页小点等离散 UI；
- 可在无 ESP-IDF 电脑上单测，后续能以 Mock RenderTarget 验证状态转换。

**还没做**：
- 真实设备 BSP/屏幕刷新的接入（P1-04）；
- 中文字形、配饰资源、LVGL 控件和 SD 图卡；
- 实际局刷窗口/鬼影/耗时测试；
- AI 语音对话与表情映射；
- 在 ESP32 上的编译与 RAM/CPU 量测。

## 接入固件的原则

正式固件可选择复用当前 Canvas1bit 模型，或把 `SceneComposer` 的语义画面映射到 LVGL。无论采用哪种，驱动层只能消费完整稳定的 framebuffer 或受控脏区，不能直接改变宠物状态；刷新队列严格序列化 SPI 访问。

不要让模型直接发送黑白图帧来做每一次互动。模型输出有限意图和表情，场景选择在本地完成。完整架构见 [ARCHITECTURE.md](ARCHITECTURE.md)。
