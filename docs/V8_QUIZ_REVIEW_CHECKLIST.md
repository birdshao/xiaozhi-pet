# V8 题库审核清单（自动生成，勿手改）

由 `python -m tools.v8_quiz_content checklist` 生成。每一条都要大人**亲耳听过**再勾选；
TTS 只是草稿，勾选之前 `approved` 保持 false。动物叫声 TODO：先用拟声兜底，后续换真人录音。

## 听声音猜动物（`animals`，content_id 201，24 题）

- [ ] `zh_c206ba2ec3.wav` (zh-CN-XiaoxiaoNeural) 喵喵喵，是谁在叫？
- [ ] `zh_285de3551f.wav` (zh-CN-XiaoxiaoNeural) 小猫
- [ ] `zh_8946c0f0c6.wav` (zh-CN-XiaoxiaoNeural) 小狗
- [ ] `zh_ce6ab3d118.wav` (zh-CN-XiaoxiaoNeural) 小鸟
- [ ] `zh_1c0ef767bc.wav` (zh-CN-XiaoxiaoNeural) 小鱼
- [ ] `zh_d63f0cfda9.wav` (zh-CN-XiaoxiaoNeural) 汪汪汪，是谁在叫？
- [ ] `zh_5259b94bd5.wav` (zh-CN-XiaoxiaoNeural) 叽叽喳喳，是谁在叫？
- [ ] `zh_3630bf33c7.wav` (zh-CN-XiaoxiaoNeural) 咕嘟咕嘟，是谁在吐泡泡？

## 大和小（`bigsmall`，content_id 202，25 题）

- [ ] `zh_99ae397d3e.wav` (zh-CN-XiaoxiaoNeural) 哪个大？
- [ ] `zh_1a12d8e7dc.wav` (zh-CN-XiaoxiaoNeural) 小的
- [ ] `zh_fb75765b86.wav` (zh-CN-XiaoxiaoNeural) 大的
- [ ] `zh_e3eea6d259.wav` (zh-CN-XiaoxiaoNeural) 哪个小？
- [ ] `zh_a813bbce2e.wav` (zh-CN-XiaoxiaoNeural) 哪个最大？
- [ ] `zh_d8e56a11a5.wav` (zh-CN-XiaoxiaoNeural) 中间的
- [ ] `zh_64954c0a6a.wav` (zh-CN-XiaoxiaoNeural) 哪个最小？
- [ ] `zh_a8d65578a7.wav` (zh-CN-XiaoxiaoNeural) 哪个是中间的？

## 英语单词卡（`english`，content_id 203，24 题）

- [ ] `en_f166362f78.wav` (en-US-AnaNeural) Where is the cat?
- [ ] `en_b921384aeb.wav` (en-US-AnaNeural) cat
- [ ] `en_2e4bfc60d3.wav` (en-US-AnaNeural) dog
- [ ] `en_b2dd344fdb.wav` (en-US-AnaNeural) bird
- [ ] `en_5645d97fd5.wav` (en-US-AnaNeural) triangle
- [ ] `en_e6434f898b.wav` (en-US-AnaNeural) square
- [ ] `en_7059687fca.wav` (en-US-AnaNeural) Where is the dog?
- [ ] `en_f85d4f6047.wav` (en-US-AnaNeural) circle
- [ ] `en_0e85715454.wav` (en-US-AnaNeural) star
- [ ] `en_31ac965828.wav` (en-US-AnaNeural) two
- [ ] `en_47df727a7f.wav` (en-US-AnaNeural) one
- [ ] `en_43d94c8c7d.wav` (en-US-AnaNeural) Where is the bird?
- [ ] `en_a5a7845d50.wav` (en-US-AnaNeural) four
- [ ] `en_2b426fc259.wav` (en-US-AnaNeural) Where is the fish?
- [ ] `en_c5bfd29c9a.wav` (en-US-AnaNeural) fish
- [ ] `en_e70f66bb98.wav` (en-US-AnaNeural) Where is the circle?
- [ ] `en_78e265428f.wav` (en-US-AnaNeural) five
- [ ] `en_e95a57f12c.wav` (en-US-AnaNeural) heart
- [ ] `en_3bdb45b64f.wav` (en-US-AnaNeural) three
- [ ] `en_12734c84f5.wav` (en-US-AnaNeural) Where is the square?
- [ ] `en_2e7b4bd5f0.wav` (en-US-AnaNeural) Where is the triangle?
- [ ] `en_02d82f9f2a.wav` (en-US-AnaNeural) Where is the star?
- [ ] `en_175e845d99.wav` (en-US-AnaNeural) Where is the heart?
- [ ] `en_799818eafc.wav` (en-US-AnaNeural) Show me one.
- [ ] `en_5f502400ca.wav` (en-US-AnaNeural) Show me two.
- [ ] `en_9c8701794d.wav` (en-US-AnaNeural) Show me three.
- [ ] `en_5f45f778da.wav` (en-US-AnaNeural) Show me four.
- [ ] `en_94430635a7.wav` (en-US-AnaNeural) Show me five.

## 内置游戏的口头提示（固件暂用提示音，TODO 录音/TTS 后接入）

- [ ] `Memory4`：找一找，哪两张是一样的？
- [ ] `Memory6`：找一找，哪两张是一样的？
- [ ] `Maze`：把团团歪一歪，帮我找到胡萝卜吧。
- [ ] `Dice`：摇一摇骰子，数一数有几个点。
- [ ] `Tree`：摇一摇小树，数一数掉下几个果子。
- [ ] `Statue`：先动一动，听到“木头人”，就一动也不动。
- [ ] `DayNight`：天黑啦，把团团扣下来睡觉；天亮啦，再翻回来。
- [ ] `Shapes`：找一找，哪个和它一样？
- [ ] `Counting`：数一数，有几个？
- [ ] `SpotDiff`：两排图里，有一个不一样，是第几个？
- [ ] `MamaBaby`：哪一个是它的宝宝？
- [ ] `SizeOrder`：大、中、小排好队，缺的是哪一个？
- [ ] `Pattern`：接下来是哪一个？
- [ ] `Classify`：哪一个能吃？哪一个不能吃？
- [ ] `Shadow`：哪一个是它的影子？
- [ ] `LeftRight`：箭头指向哪一边，就选哪一边。
- [ ] `Clock`：现在是这个时间，我们该做什么？
- [ ] `Weather`：今天天气这样，团团该穿什么？
