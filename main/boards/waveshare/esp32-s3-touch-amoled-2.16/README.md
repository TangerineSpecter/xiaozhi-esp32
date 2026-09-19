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
