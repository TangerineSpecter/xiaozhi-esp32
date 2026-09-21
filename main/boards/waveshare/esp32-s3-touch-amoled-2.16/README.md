# Waveshare ESP32-S3-Touch-AMOLED-2.16


[ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm) ESP32-S3-Touch-AMOLED-2.16 is a high performance, highly integrated microcontroller development board designed by Waveshare.\
In the smaller form, the 2.16-inch capacitive HD AMOLED screen, highly integrated power management chip, six-axis sensor (three-axis accelerometer and three-axis gyroscope), RTC, low-power audio codec chip and echo cancellation circuit are mounted on the board, which is convenient for development and embedding into the product.

## 本地编译与烧录

在 macOS 上为本开发板编译和烧录 XiaoZhi 固件，请参阅：[Waveshare ESP32-S3-Touch-AMOLED-2.16 烧录手册](../../../../docs/waveshare-esp32-s3-touch-amoled-2.16-flashing_zh.md)。


## 宠物界面布局

- 480×480 屏幕左上角使用 164×44 的体力胶囊：爱心、0–100 数值和细进度条；充电时仅显示电池闪电图标。
- 右上角显示紧凑状态提示，对话使用其下方的 212×82 气泡；人物保持 300×300、底部居中，底部不再覆盖字幕条。
- 本板默认正文和图标使用 20px 字体。对话按实际字体测量为两行一页，每 4 秒切换一页，最后一页保留至替换或清空；这是定时分页，并非逐字语音同步。
- 新消息替换旧分页，清空对话或进入聆听时停止并清除旧分页；隐藏字幕时暂停，重新显示时从第一页开始。长状态和通知使用横向滚动。
- 待机、聆听、说话、充电切换、低电量、长句和字幕隐藏仍需在实机确认可读性及换页节奏。

## images

| [ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm)                                                                                                                                                                                           |
|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-2.16-1.jpg"> |
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-2.16-3.jpg"> |
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-2.16-4.jpg"> |

## 三键设置菜单

- 中间 `PWR` 短按打开菜单，再短按确认；长按约 2 秒返回上一级。
- 上方 `GPIO18` / 下方 `BOOT` 在 2×2 菜单中循环选择，当前提供“音量”“录音／停止录音”“记录／停止记录”和“返回”。
- 进入音量页后，上键增加、下键减少，每次 5%，范围 0–100%；中键确认后才应用并写入原有 `audio/output_volume` 设置。0% 静音在本板重启后保留。
- 音量页长按中键 2 秒取消未保存的修改。保存后回到菜单，再选择“返回”或长按退出。
- 菜单使用遮罩和浅紫圆角图标卡片，180ms 淡入并轻微上浮。屏幕触摸只用于休眠唤醒；正常亮屏时，菜单外 BOOT 的对话、双击 AEC（启用时）及上下键组合重启保持原行为。
- 中键仍是 AXP2101 电源键，继续按住约 4 秒仍会硬件关机；返回后请松手。2 秒仅修改 IRQLEVEL，保留原有开关机阈值。
- 按键通过独立任务每 50ms 读取 AXP2101 的 `0x49` 状态（短按 bit 3、长按 bit 2），仅清除已读取的按键标志。应用队列最多保留一个电源键处理回调。寄存器定义参考项目内 XPowersLib 的 `XPowersParams.hpp` / `XPowersAXP2101.hpp`。

## 屏幕省电与唤醒

- 电池供电空闲 60 秒后进入轻度省电，亮度降到 20%；单击屏幕或任一实体键只唤醒屏幕，不继续执行聊天、配网或菜单动作。
- 持续空闲到 300 秒后亮度降到 5%，设备仍保持运行；屏幕需要在 700ms 内连续点击两次才能唤醒，两次点击不要求落在相同位置。
- 持续空闲到 600 秒后由 AXP2101 真正关机；关机后触摸和 GPIO 按键不再工作，只能短按中间 `PWR` 键开机。
- 屏幕手势只负责唤醒，不会同时启动对话。唤醒、开始对话或其他活动都会重新计算上述空闲时间。

实机验收：短按打开/确认/保存，2 秒返回后松手不误确认，4 秒关机，连续上下键、0/100 边界、取消不改变声音、保存后重启（含静音）、60 秒单击唤醒、300 秒双击唤醒、600 秒关机、对话过程中打开菜单，以及组合键重启。编译及主机测试不能替代这些检查。

## TF 卡录音与文字记录

本板使用独立的 `SPI3_HOST` 驱动 TF 卡，避免与屏幕使用的 `SPI2_HOST` 冲突。接线固定为 `GPIO1` MOSI/CMD、`GPIO2` CLK、`GPIO3` MISO/D0、`GPIO41` CS。开机挂载失败不会阻止设备启动，首次使用录音或记录时会再次尝试挂载；固件不会自动格式化 TF 卡。

- 录音保存为 16 kHz、16-bit、单声道 PCM WAV：`/sdcard/xiaozhi/audio/YYYYMMDD/HHMMSS.wav`。
- 文字记录仅保存服务器返回的用户最终 STT，不保存小智回复：`/sdcard/xiaozhi/notes/YYYYMMDD/HHMMSS.txt`。
- 未同步时间时文件名使用 `boot-<启动毫秒>-<序号>`；已有同名文件时追加 `-01`、`-02` 等后缀。
- 可以在菜单中操作，也可以唤醒小智后说“帮我录音／开始录音／停止录音”或“帮我记录一下／停止记录”。语音控制依赖联网 STT 和服务器 MCP 工具调用，不包含离线命令词识别。
- 录音与文字记录可独立或同时运行。录音中显示麦克风角色和 `REC mm:ss`；文字记录中显示记录状态。任一功能运行时暂停屏幕自动休眠和十分钟自动关机，全部停止后恢复原省电策略。
- SD 写入由独立低优先级任务处理，音频先进入 PSRAM 缓冲区。剩余空间低于 32 MB、WAV 接近 4 GB、缓冲区溢出或发生拔卡/I/O 错误时会安全停止并提示。
- WAV 文件头每 10 秒刷新并同步，正常停止时再次回写。异常断电仍可能丢失最后一个刷新周期的数据，需要在实机上验证所用 TF 卡的行为。

当前版本不提供录音回放、文件浏览、删除、上传或录音文件转文字。TF 卡、麦克风通道、长时间录音、热插拔与掉电恢复都必须经过实机验收；固件编译成功不能替代硬件验证。
