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
- 上方 `GPIO18` / 下方 `BOOT` 选择图标，当前提供“音量”和“返回”。
- 进入音量页后，上键增加、下键减少，每次 5%，范围 0–100%；中键确认后才应用并写入原有 `audio/output_volume` 设置。0% 静音在本板重启后保留。
- 音量页长按中键 2 秒取消未保存的修改。保存后回到菜单，再选择“返回”或长按退出。
- 菜单使用遮罩和浅紫圆角图标卡片，180ms 淡入并轻微上浮。菜单期间屏幕双击不会启动对话；菜单外 BOOT 的对话、双击 AEC（启用时）及上下键组合重启保持原行为。
- 中键仍是 AXP2101 电源键，继续按住约 4 秒仍会硬件关机；返回后请松手。2 秒仅修改 IRQLEVEL，保留原有开关机阈值。
- 按键通过独立任务每 50ms 读取 AXP2101 的 `0x49` 状态（短按 bit 3、长按 bit 2），仅清除已读取的按键标志。应用队列最多保留一个电源键处理回调。寄存器定义参考项目内 XPowersLib 的 `XPowersParams.hpp` / `XPowersAXP2101.hpp`。

实机验收：短按打开/确认/保存，2 秒返回后松手不误确认，4 秒关机，连续上下键、0/100 边界、取消不改变声音、保存后重启（含静音）、休眠唤醒、对话过程中打开菜单，以及原有双击屏幕和组合键重启。编译及主机测试不能替代这些检查。
