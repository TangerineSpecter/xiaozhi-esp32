# AGENTS.md

## 项目

小智（XiaoZhi）是基于 ESP-IDF 的 C/C++ 语音助手固件，支持多种芯片、开发板、显示屏、音频设备和网络传输方式。一次构建只能选择一个开发板实现。

优先使用 ESP-IDF 6.1。最低支持版本为 ESP-IDF 6.0.1，不支持 IDF 5.x。

## 架构

- `main/application.*`：主事件循环、协议生命周期和高级行为。
- `main/device_state_machine.*`：合法的运行时状态转换。
- `main/boards/common/`：开发板接口以及可复用的硬件、网络辅助组件。
- `main/boards/**/`：开发板专属引脚、初始化逻辑和构建变体。
- `main/audio/`：编解码器、音频任务、音频引擎、唤醒词和队列。
- `main/protocols/`：与传输方式无关的 API，以及 WebSocket 和 MQTT/UDP 实现。
- `main/display/` 和 `main/led/`：可复用的 UI 实现。
- `main/mcp_server.*`：设备侧通用 MCP 工具和分发逻辑。
- `main/Kconfig.projbuild`：开发板和功能配置。
- `main/CMakeLists.txt`：源码、开发板、语言、字体和资源选择。
- `scripts/build.py`：规范的开发板/变体构建入口。

新增实现前先阅读最近的现有实现。优先放在最窄的所属层级，不要把开发板专属行为放进核心模块。

## 必须遵守的规则

- 保留工作区中与当前任务无关的修改，补丁应保持聚焦。
- 一次构建必须且只能通过 `DECLARE_BOARD(...)` 导出一个开发板工厂。
- 不要修改已有开发板的引脚来适配不同硬件。应新增名称唯一的开发板或发布变体；开发板身份会影响 OTA 兼容性。
- 核心代码只能依赖 `Board` 接口，不得依赖具体开发板类或开发板的 `config.h`。
- 摄像头、背光、显示屏、LED、电池等能力都必须按可选能力处理。
- 通过 `Application::SetDeviceState()` 和状态机修改运行时状态。
- 回调可能运行在主任务之外。应用层状态变更必须使用 `Application::Schedule()` 或事件位进行调度。
- 不要阻塞主事件循环或音频任务。避免无界队列以及在音频路径中反复进行大块内存分配。
- 共享消息语义必须放在 `Protocol` 中；修改协议契约时要同时检查两种传输方式。
- 校验网络输入并正确维护 `cJSON` 所有权。NVS key 属于持久化 API，修改时必须考虑迁移。
- 使用 Kconfig/组件规则保护目标专属功能。不能假设所有目标都具备 PSRAM 或 S3/P4 资源。
- 不要手动修改生成文件或供应商输出：`build/`、`releases/`、`managed_components/`、`components/`、`sdkconfig*`、`main/assets/lang_config.h` 以及生成的 mmap 头文件。
- 只使用仓库的 `.clang-format` 格式化本次修改涉及的 C/C++ 文件，避免无关的大范围格式化。

## Waveshare ESP32-S3-Touch-AMOLED-2.16 的 HTML 原型规则

`main/boards/waveshare/esp32-s3-touch-amoled-2.16/preview/` 下的 HTML 页面是该开发板 UI 的交互和视觉原型，当前包含 `index.html`、`styles.css`、`app.js` 和 `README.md`。

- ESP32-S3-Touch-AMOLED-2.16 的硬件 UI、菜单布局、触摸操作、实体按键映射、状态显示、动画反馈、录音/记录状态和错误提示，都应与 HTML 原型保持一致。
- 后续优先在 HTML 原型中确认交互和视觉效果，再同步到硬件；硬件实现不能自行引入与原型不一致的交互或状态语义。
- 修改硬件 UI 或交互时，必须同步检查 HTML 原型；修改 HTML 原型时，也要确认硬件端能够实现相同的操作流程和反馈。
- HTML 原型是该开发板 UI 验收时的参考基准。若受到屏幕尺寸、输入设备或硬件能力限制而无法完全一致，必须在原型和硬件中明确记录并保持可理解的一致行为。
- 角色资源、菜单文字、触摸热区、按钮行为和状态优先级应以 HTML 原型为准，不要只在固件中维护另一套隐含规则。

## 开发板与配置

开发板选择是一个相互关联的链路：

`config.json` -> `scripts/build.py` -> `main/Kconfig.projbuild` -> `main/CMakeLists.txt` -> 开发板源码和 `config.h`。

新增开发板或变体时，必须更新这条链路中所有相关位置。需要包含唯一的开发板身份、正确的芯片目标、Flash/分区设置、且只能有一个 `DECLARE_BOARD`，并补充开发板文档。遵循 `docs/custom-board.md`。

## 命令

先加载指定的 ESP-IDF 环境：

```sh
source /path/to/esp-idf/export.sh
idf.py --version
```

在同一个构建目录中始终使用已激活环境里的 `python`。不要在同一个构建目录混用 `python` 和 `python3`：ESP-IDF 会把解释器的绝对路径写入构建配置，切换名称可能需要执行 `idf.py fullclean`。

```sh
# 查看准确的开发板和变体名称
python scripts/build.py --list-boards

# 构建规范的开发板变体
python scripts/build.py <board-directory> --name <variant-name>

# 运行主机侧构建测试
python -m unittest discover -s scripts/tests -v

# 格式化/检查涉及的文件
clang-format -i <files>
clang-format --dry-run -Werror <files>
```

构建脚本会修改本地 `sdkconfig` 和构建状态。不要假设构建目录仍然对应之前的目标。

## 验证

- 仅开发板修改：构建受影响的变体，并对修改的硬件行为进行冒烟测试。
- 核心、公共开发板、音频、协议、显示、依赖、Kconfig 或 CMake 修改：运行主机测试，并构建受影响的代表性芯片/网络路径。
- 协议修改：共享行为发生变化时，同时验证 WebSocket 和 MQTT/UDP。
- 音频修改：验证采集、播放、唤醒/VAD、中断、重连以及适用的 AEC 模式。
- UI/资源修改：验证适用的无显示/OLED/LVGL 路径和分区大小，并用对应 HTML 原型检查交互和视觉状态。
- 始终报告已经测试的内容和仍需实机验证的内容。构建成功不等于硬件验证完成。

## 权威文档

- 项目概览和 SDK 政策：`README.md`
- 开发板指南：`docs/custom-board.md`
- 音频设计：`main/audio/README.md`
- 代码风格：`docs/code_style.md`
- 协议文档：`docs/websocket.md`、`docs/mqtt-udp.md`、`docs/mcp-protocol.md`
- CI 矩阵：`.github/workflows/build.yml`
- Waveshare ESP32-S3-Touch-AMOLED-2.16 资源：[官方资源与文档](https://docs.waveshare.net/ESP32-S3-Touch-AMOLED-2.16/Resources-And-Documents)

详细或变化较快的信息应放在这些文档中，而不是本文件。只有当某个子系统需要专门规则时，才新增嵌套的 `AGENTS.md`。
