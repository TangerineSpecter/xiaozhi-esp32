# XiaoZhi 角色表情资源

这些 PNG 是 Waveshare ESP32-S3-Touch-AMOLED-2.16 使用的板级表情资源，构建时会自动打包进默认 `assets.bin`。

文件名必须与 XiaoZhi 的 emotion 名称一致：

- `neutral.png`：普通
- `happy.png`：开心
- `thinking.png`：思考
- `sad.png`：难过
- `angry.png`：生气
- `sleepy.png`：睡觉

当前资源规格为 `64×64`、RGBA PNG。新增表情时，需要同时确认服务端下发的 emotion 名称和固件资源文件名一致。
