# XiaoZhi 角色表情资源

这些 PNG 是 Waveshare ESP32-S3-Touch-AMOLED-2.16 使用的板级表情资源，构建时会自动打包进默认 `assets.bin`。

文件名必须与 XiaoZhi 的 emotion 名称一致：

- `neutral.png`：普通
- `happy.png`：开心
- `thinking.png`：思考
- `sad.png`：难过
- `angry.png`：生气
- `sleepy.png`：睡觉
- `petting.png`：点击角色头部时短暂显示的闭眼微笑表情
- `recording.png`：录音中，角色将麦克风递向屏幕外的用户；REC 点、声波、计时和状态徽标由 LVGL 单独绘制
- `taking_notes.png`：文字记录中，角色手持笔记本和笔进行记录；“记录中”徽标由 LVGL 单独绘制
- `dizzy.gif`：摇晃触发的眩晕动画，以 `neutral.png` 原始像素生成全部帧

当前静态资源规格为 `108×108`、RGBA PNG；`dizzy.gif` 为 `108×128`，
20 帧循环、每帧 100 ms，一轮 2 秒。角色主体直接使用 `neutral.png` 的像素，
视频仅用于测量身体各部分的运动和星环位置，不用作角色纹理，因此衣服、头发和面部
保持原图的颜色、线条与清晰度。眩晕状态持续 2 秒，正好播放一轮。
上方增加 12 行透明区域容纳星星；下方增加 16 行衣料，其中 8 行写入 GIF，
另外 8 行只在生成旋转帧时作为隐藏余量。衣料使用 `neutral.png` 最后几行的镜像像素，
保持原调色板并避免接缝。显示端将 GIF 向下补偿 8 个源像素，原衣服位置不变。
可用 `python main/boards/waveshare/esp32-s3-touch-amoled-2.16/assets/animation/generate_dizzy_flow.py <参考视频>`
重新生成（需 OpenCV、NumPy 和 FFmpeg）。
`../animation/dizzy_frames/` 保存全部 20 帧。
`../animation/dizzy-preview.png` 每隔 3 帧采样一张；
`../animation/dizzy-preview.gif` 展示完整动画。两者均由最终 GIF 解码生成。
新增普通表情时，需要同时确认服务端下发的 emotion
名称和固件资源文件名一致；`dizzy` 由板级摇晃状态直接触发。
