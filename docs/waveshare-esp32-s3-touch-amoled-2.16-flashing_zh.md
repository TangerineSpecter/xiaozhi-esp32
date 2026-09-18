# Waveshare ESP32-S3-Touch-AMOLED-2.16 烧录手册

本文适用于在 macOS 上使用本地 XiaoZhi 源码，为
`waveshare/esp32-s3-touch-amoled-2.16` 开发板编译并烧录固件。

官方硬件资料：

- [Resources and Documents](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/Resources-And-Documents)
- [Instructions for Use](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/Instructions-For-Use/)

## 一、准备工作

确认 USB 数据线可以传输数据，并在项目根目录的终端中执行下面的命令。

加载 ESP-IDF 6.1 环境：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py --version
```

版本建议为 `v6.1`。项目最低支持 ESP-IDF 6.0.1，不支持 ESP-IDF 5.x。

加载环境后，构建和烧录过程统一使用 `idf.py` 和 `python`。不要在同一次构建中混用 `python` 与 `python3`，否则可能出现 ESP-IDF 记录的 Python 解释器路径不一致。

注意：命令中的 `~` 前面不要加反斜杠。应写成：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
```

不要写成 `source \~/.espressif/...`，否则 Shell 可能无法展开用户目录。

## 二、配置并编译固件

在仓库根目录执行：

```bash
python scripts/build.py waveshare/esp32-s3-touch-amoled-2.16
```

这个脚本会自动完成开发板配置、编译和合并固件，但不会自动把固件写入设备。

如果看到类似下面的提示：

```text
the project was configured with .../venv/bin/python3
Run 'idf.py fullclean' to start again
```

先清理旧的构建目录，再使用同一个 `python` 解释器重新构建：

```bash
idf.py fullclean
python scripts/build.py waveshare/esp32-s3-touch-amoled-2.16
```

编译成功后检查输出文件：

```bash
ls -lh build/merged-binary.bin
```

看到该文件后，说明构建阶段已经完成。`build/merged-binary.bin` 是合并后的固件文件；下面的 `idf.py flash` 会根据当前 `build` 配置烧录对应的启动程序、分区表和应用固件。

如果只是修改了源码，例如本项目新增的“同时按住上下键 10 秒强制重启”功能，必须重新执行本节的编译命令，不能直接烧录旧的 `build` 输出。

## 三、查看开发板串口

保持开发板通过 USB 连接，在另一个终端或当前终端执行：

```bash
ls /dev/cu.usb*
```

通常会看到类似：

```text
/dev/cu.usbmodemXXXX
```

记下实际显示的完整端口名。每次重新插拔 USB 后，`XXXX` 可能发生变化。

## 四、烧录并查看运行日志

将下面的端口名替换为实际端口：

```bash
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

这条命令会依次烧录固件，然后打开串口监视器。烧录完成后设备会自动重启，可以在日志中观察启动、联网和配网状态。

退出串口监视器通常使用：

```text
Ctrl-]
```

## 五、烧录连接不上时

如果出现无法连接串口、等待下载模式或 `Failed to connect`，按以下顺序操作：

1. 按住开发板的 `BOOT` 键；
2. 按一下 `RESET`/`EN` 键（如果当前外壳或板上可以操作）；
3. 松开 `BOOT` 键；
4. 再次执行烧录命令：

   ```bash
   idf.py -p /dev/cu.usbmodem1101 flash monitor
   ```

如果串口被其他程序占用，先关闭其他串口工具或之前打开的 `monitor`，再重新执行命令。

## 六、烧录后的功能确认

烧录完成后，按以下顺序确认：

1. 设备正常启动，屏幕不再停留在旧的配网状态；
2. 按照 XiaoZhi 提示完成 Wi-Fi 配置；
3. 确认设备能够唤醒并进行对话；
4. 对本项目新增的紧急重启功能进行测试：
   - 同时按住最上方按钮和最下方 `BOOT` 按钮；
   - 连续保持约 10 秒；
   - 松开两个按钮；
   - 设备执行强制重启。

中间的 `PWR` 按钮不参与这个组合键。该功能是重启，不是切断锂电池供电；不会清除已保存的 Wi-Fi 配置。

## 七、不要误用的命令

除非明确要清空设备数据，否则不要执行：

```bash
idf.py erase-flash
```

擦除整个 Flash 可能会清除 NVS 中保存的 Wi-Fi 配置及其他设备数据。普通源码更新只需要重新编译并执行 `flash`，不需要先擦除 Flash。

## 八、最短操作版

以后只需要记住下面这组命令：

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py --version
python scripts/build.py waveshare/esp32-s3-touch-amoled-2.16
ls /dev/cu.usb*
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```
