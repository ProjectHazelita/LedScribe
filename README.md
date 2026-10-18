<!-- markdownlint-disable MD033 MD041 -->

<div align="center">

# LedScribe

**把想说的话，写进 11 行光里。**

为 CH546 11×44 LED 胸牌准备的 Linux 命令行写入工具。

[快速开始](#-快速开始) · [使用说明](#-使用说明) · [开发与测试](#-开发与测试) · [许可说明](#-许可说明)

</div>

## 📖 关于

一块小小的 LED 胸牌，也该能在 Linux 上好好写字。

LedScribe 面向 USB 标识为 `0416:5020`、使用 `wch.cn CH546` 的 11×44 LED 胸牌。从终端输入文字，先看看点阵，再把消息写进胸牌。不需要先启动商家的图形化软件。

这是一个从早期练手项目继续整理而来的小工具，使用 C++20、libusb 和 stb_truetype，主要面向 Fedora/Linux；它不是通用的 LED 屏驱动，也不承诺适配所有外观相似的胸牌。

- **一至八条消息**：按槽位设置速度、模式、闪烁和边框，统一调整亮度。
- **两种文字渲染路径**：商家 `ASC11` / `HZK11` 点阵字库，以及普通 TTF/TTC 轮廓字体。
- **先看，再写**：终端预览完整位图；`--dry-run` 只渲染和编码，不访问 USB。
- **把问题说清楚**：显式字库路径有误或轮廓字体缺字时报告错误，不悄悄忽略。

> [!NOTE]
> 项目正在重构。已在一块 `0416:5020 / CH546` 胸牌上完成基础写入、八槽位轮播和八种模式的实机回归；四向移动、静止、闪烁、边框及 drop/curtain/laser 效果经目视确认。采样的 12 个返回包均为完整的 64 字节回显。
>
> `animation` 的两帧与四帧播放已完成实机复核：逐帧补齐修复后，各帧显示完整，不再累计左偏。分槽速度及 44 列中文静态显示也已确认。其他硬件型号、设备容量边界及缩短发送间隔尚未验证。

## 🚀 快速开始

先完成一次**不需要胸牌、不需要 USB 权限**的预览。

### 准备环境

推荐使用仓库提供的 Nix 开发环境，需要已启用 Flakes 的 Nix：

```bash
git clone https://github.com/ProjectHazelita/LedScribe.git
cd LedScribe
nix develop
```

已有源码时，直接在仓库根目录执行 `nix develop` 即可。首次准备环境需要联网，CMake 配置时还会从 GitHub 下载 `stb_truetype.h`。

不用 Nix 也可以：自行准备支持 C++20 的 Clang、CMake ≥ 3.25、Ninja、pkg-config 与 libusb 1.0 开发文件，再使用下面的命令。`dev` preset 使用 `clang++` 和 Ninja。

### 构建并预览

以下命令均在**仓库根目录**执行。示例使用 `vendor/` 中的商家字库，其授权边界见[许可说明](#-许可说明)。

```bash
cmake --preset dev
cmake --build --preset dev
./build/ledscribe --vendor-font-dir ./vendor --preview --dry-run "洛汐"
```

终端会显示一张 `22×11` 的位图，并以这行输出结束：

```text
encoding complete: 128 bytes, 2 CH546 reports
```

到这里，字体加载、渲染和协议编码就跑通了，没有向 USB 设备写入任何数据。若源码目录曾改名、配置时报旧路径错误，见[旧名称迁移](#-旧名称迁移)。

## 📝 使用说明

### 连接胸牌，准备 USB 权限

Linux 下，首次使用可安装仓库提供的精确匹配规则。**下面两条命令需要管理员权限，会安装系统 udev 规则并重载规则配置：**

```bash
sudo install -Dm644 udev/70-ledscribe.rules /etc/udev/rules.d/70-ledscribe.rules
sudo udevadm control --reload-rules
```

拔下并重新插入胸牌，然后列出设备：

```bash
./build/ledscribe --list-devices
```

规则使用 `uaccess`，面向活跃的本地桌面会话，并非让所有用户都能访问设备。SSH 会话或系统服务的权限需要另外安排，不建议直接改成全局可写。

### 预览一组消息，再决定是否写入

```bash
./build/ledscribe --vendor-font-dir ./vendor \
  --speed 3,6 --mode left,still \
  --blink 2 --border 1 --brightness 50 \
  --preview --dry-run "你好，世界" "LedScribe"
```

这会为第一条设置速度 3、左移和边框，为第二条设置速度 6、静止和闪烁；两条共用 50% 亮度。

`--preview` 展示的是**完整的 11 行位图**，不是 44 列屏幕视口或动画模拟。上例中的 `LedScribe` 使用商家字库时宽于 44 列，完整预览不代表静止模式能同时显示全部内容。

准备好后，以下命令会真正向连接的胸牌写入两条消息：

```bash
./build/ledscribe --vendor-font-dir ./vendor "洛汐" "LUOXI OK"
```

只连接一块兼容设备时会自动选择；多设备时，需要用 `--device` 指定 `--list-devices` 输出中的完整 ID，格式为 `BUS:DEV:OUT:IN`。写入后若仍显示充电画面，可拔掉 USB 或按 USB 接口旁的按钮查看。

已测试的胸牌有独立的电源按钮和内容切换按钮。内容按钮会在**轮换播放 → 单独播放槽位 1 → 单独播放槽位 2 → ……**之间切换：只有轮换播放才会按顺序播放各槽位，并在末尾回到第一条；单槽位模式持续显示或播放该槽位，不会自动进入下一条。动画的帧切换发生在同一槽位内部，不等于切换槽位。

### 常用参数

| 参数 | 用途与取值 |
| --- | --- |
| `--speed N[,N...]` | 速度 `1..8`，`1` 最慢、`8` 最快，默认 `4`；分槽速度已实机确认 |
| `--mode MODE[,MODE...]` | `left`、`right`、`up`、`down`、`still`、`animation`、`drop`、`curtain`、`laser`；默认 `left` |
| `--brightness N` | `25`、`50`、`75`、`100`，默认 `50` |
| `--blink SLOT[,SLOT...]` | 闪烁槽位，使用 `1..8` 或 `all` |
| `--border SLOT[,SLOT...]` | 边框槽位，使用 `1..8` 或 `all` |
| `--font PATH` | 指定受信任的 TTF/TTC 文件，跳过商家字库 |
| `--font-size N` | 初始字号，必须大于 `5`，默认 `11`；必要时缩小以适应 11 行高度 |
| `--threshold N` | 二值化阈值 `0..255`，默认 `96` |
| `--vendor-font-dir PATH` | 指定同时包含 `ASC11` 与 `HZK11` 的目录 |
| `--no-vendor-font` | 禁用商家字库，使用轮廓字体 |

`--speed`、`--mode` 的单个值作用于全部槽位；多个值按顺序设置，未指定的槽位保留默认值。列表最多八项，空项和越界值会报错。模式也可使用数字 `0..8`，对应表中列出的名称顺序。完整帮助见 `./build/ledscribe --help`。

### 动画如何分帧

`animation` 使用同一槽位的宽位图切换画面，不是把消息列表中的多个槽位当作动画帧。单帧内容看起来静止是正常现象。

当前编码器按**渲染后的每 44 列**切帧，每帧独立补齐至 48 列存储，右侧的 4 列保持熄灭；末帧不足 44 列的部分也补暗。不能只在整条消息末尾补齐，否则后续帧会累计向左错位。其他显示模式仍使用连续位图，不插入这些帧间留白。

例如，商家字库的四个汉字恰好占 44 列，下面的文字可以生成两帧。轮廓字体或中英混排的字宽不同，不保证按字符边界分帧：

```bash
./build/ledscribe --vendor-font-dir ./vendor --mode animation \
  --speed 4 --preview --dry-run "第一帧图第二帧图"
```

终端预览仍展示原始完整位图，不模拟动画播放；分帧后的留白与容量计算在封包阶段处理。

### 字体如何选择

没有指定 `--font` 或 `--no-vendor-font` 时，程序优先寻找商家字库：未提供目录则查找**当前工作目录**下的 `vendor/`；显式指定的目录不完整则直接报错，不再尝试其他目录。

商家字库路径下，遇到无法用 GBK 编码的字符时，会让**整条消息**回退到轮廓字体。因此，加入一个不支持的字符，也可能改变整条消息的中文字形。

轮廓字体路径下，可以通过 `--font` 指定文件；不指定时程序会检查若干预设的系统字体路径，并输出实际选中的字体。自动找到文件不代表它覆盖全部文字：缺字时会报告 `U+XXXX` 并停止，请换用覆盖整条消息的字体。`--font` 和 `--no-vendor-font` 路径不使用 `--vendor-font-dir`。

当前渲染不保证复杂文字 shaping、双向排版或 emoji 序列排版正确。字号和阈值的范围始终会检查，即使本次最终使用商家字库。

## 💻 开发与测试

### Qt Widgets 编辑器（可选）

CLI 和核心库不依赖 Qt；默认构建不查找 Qt。启用原生中文编辑器：

```bash
nix develop
cmake --preset gui -DLEDSCRIBE_WARNINGS_AS_ERRORS=ON
cmake --build --preset gui
./build/gui/ledscribe-gui
ctest --preset gui
```

编辑器支持 1–8 条消息、内部拖动排序、逐条模式/速度/闪烁/边框、全局亮度与字体设置。无需设备即可编辑、预览和保存。空文字及未选字体允许保存为草稿，但会显示渲染错误；超容量项目保留预览并显示实际字节数。字体资源只存路径、不嵌入工程，加载相对路径时以工程目录为基准。

“文件”菜单提供新建、打开、保存与另存为，工程格式为 `*.ledscribe.json`。打开失败不替换当前工程；保存取消或失败不清除未保存状态。预览为真实像素：动画每帧恰 44×11，末帧补暗，不显示存储用的 4 列 padding；其他模式显示完整条带。滚动、闪烁、边框和特效由设备执行，预览不猜测播放帧率。字体缺失或当前消息出错时清空旧图。

文字和字体路径可在中间继续编辑，预览刷新不重置光标、选区或撤销历史。输入法的预编辑文字不进入工程；提交候选后才更新模型与预览，不把同值文字回写到正在组合输入的编辑框。

写入设备：

1. 连接胸牌后点“刷新设备”。启动、编辑与预览不会自动访问 USB；刷新只读取描述符，不占用接口，也不代表已有访问权限。
2. 选择目标设备。仅一个可用目标时自动选中；多个目标必须明确选择，描述符错误项不可写。重连后必须刷新，不会沿端口静默跟随变化的设备地址。
3. 全部消息渲染成功且未超容量后，点“写入设备”，在默认取消的确认框中核对完整设备 ID、消息数和字节数。写入使用这一份预览快照，不重新取控件文字或重渲染。
4. 写入期间编辑、文件动作和设备选择暂停，窗口继续响应；进度仅在每个 64 字节报告的 OUT 与完整 ACK 成功后增加。写入中不能关闭窗口，不提供中途取消或自动重试。
5. “传输完成”表示 USB 传输完成，并非电脑已经目视验证了胸牌。请用胸牌的内容按钮选择轮换或单槽查看；电源按钮只负责开关，不改变两者分工。

拔线、权限不足、设备占用或超时会显示底层原因；设备可能已经收到部分内容。工程仍保留，失败后需刷新并重新选择，再由用户确认重写，不能直接续写旧目标。

已在真实胸牌上验证两帧／四帧动画与静止消息的 576 字节、9 报告传输及轮换播放；拔线实测在 2/9 报告后返回 `LIBUSB_ERROR_NO_DEVICE`，保留工程、禁用旧目标且未自动重试。原生 Wayland/X11 窗口、拖动排序、字体错误、另存为和重开由现场交互与截图确认。硬件表现依具体设备而异，不能用传输成功代替实际观察。

Nix 开发环境包含 Qt 6 Widgets、Qt Test、Wayland / X11 平台插件，以及与本套 Qt 匹配的 Fcitx5 输入上下文插件。可检查 Qt 是否来自该环境：

```bash
nix develop
pkg-config --modversion Qt6Core Qt6Gui Qt6Widgets
qmake -query QT_VERSION
```

使用仓库 `.envrc` 自动加载环境时，可执行 `direnv reload`，让交互式 shell 重新加载依赖。已经运行的进程不会自动继承新的环境变量。

开发 shell 设置了 Nix Qt 模块对应的 `QT_PLUGIN_PATH`，让本地编译、尚未经过 Nix 打包包装的程序也能找到平台插件；不要将它替换为系统 Qt 的插件目录。没有全局强制显示后端，调试时可用 `QT_QPA_PLATFORM=wayland` 或 `QT_QPA_PLATFORM=xcb` 显式选择。

使用 Fcitx5 时沿用桌面提供的 `QT_IM_MODULE=fcitx`，shell 会把匹配的 Qt 插件加入查找路径，不强制其他用户切换输入法。若能输入英文却没有中文候选，请检查输入法服务与当前进程的插件环境；不要混入不同 Qt 版本的系统插件。修改环境后需重新进入 shell 并重启 GUI，已打开的窗口不会获得新插件。

GUI 的离线测试覆盖工程文件校验/原子保存、消息移动、像素分页和快照失效；独立 sanitizer 构建可使用 `gui-sanitizers` configure/build/test preset。原生窗口可分别通过 Wayland 和 X11 后端启动；离屏测试不代替实际窗口和硬件目视验收。

### 离线回归

常规离线回归：

```bash
ctest --preset dev
```

测试不连接胸牌，也不依赖系统默认字体，覆盖位图打包、UTF-8 校验、商家字库、模式序列化、字体目录发现和 CLI 参数边界。编译数据库生成在 `build/compile_commands.json`，供 clangd 使用。

<details>
<summary>严格警告与 Sanitizer 检查</summary>

将编译器警告视为错误：

```bash
cmake --preset dev -DLEDSCRIBE_WARNINGS_AS_ERRORS=ON
cmake --build --preset dev
```

在允许 LeakSanitizer 检查进程的普通终端中，使用独立的 `build/sanitizers` 构建目录：

```bash
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

该 preset 启用 ASan、UBSan、LSan 及严格警告，检测到相应错误会令测试失败。离线测试不代替实际 USB 传输、显示效果或发送时序验证。

</details>

### 实现入口

- [`src/main.cpp`](./src/main.cpp)：参数解析与应用流程。
- [`src/vendor_font.cpp`](./src/vendor_font.cpp)、[`src/truetype.cpp`](./src/truetype.cpp)：两种字体渲染路径。
- [`src/render.cpp`](./src/render.cpp)：共享字体选择、整条消息回退与字体来源元信息，核心不依赖 Qt。
- [`src/pack.cpp`](./src/pack.cpp)、[`src/protocol.cpp`](./src/protocol.cpp)：位图打包、模式名称与协议头；`measure_payload` 在分配封包前计算容量，动画每帧44列、存储48列，最终按64字节对齐。合法超容量布局仍可测量，但禁止构建传输数据。
- [`src/device.cpp`](./src/device.cpp)：描述符发现与独立 RAII 写入会话。列出设备不打开或占用接口，也不证明有访问权限；写入前重新核对设备地址、端口和接口身份，重连后需刷新并重新选择。进度仅在每个报告完成 OUT 与完整 ACK 后增加；当前返回包只校验传输结果与长度，不解析内容，不自动重试。

## 🔧 旧名称迁移

项目原名 **LEDyRochen**，原仓库位于 `LyCecilion`，现迁移至 **ProjectHazelita/LedScribe**。命令、CMake 目标和 udev 规则已统一使用新名称，不保留旧命令别名。

<details>
<summary>本地目录改名后，CMake 仍引用旧绝对路径</summary>

先停止正在进行的构建。下面只移除生成的 stb 子构建目录，再重新生成主构建缓存，不会清空整个 `build/`：

```bash
cmake -E rm -rf build/_deps/stb-subbuild
cmake --preset dev --fresh
cmake --build --preset dev
```

若使用过 sanitizer 构建，对 `build/sanitizers/_deps/stb-subbuild` 和 `sanitizers` preset 做相同处理。旧目录中可能仍有 `ledyrochen` 二进制，请运行新命令 `ledscribe`，不要误用旧产物。

</details>

<details>
<summary>本地远程地址或系统 udev 规则仍使用旧名称</summary>

Git 远程地址不会随网页上的仓库转移自动更新。需要时执行：

```bash
git remote set-url origin git@github.com:ProjectHazelita/LedScribe.git
```

如果之前手动安装过 `/etc/udev/rules.d/70-ledyrochen.rules`，在新规则安装完成后移除旧文件，再重载规则。源码中的文件改名不会替你修改系统已安装的规则。

</details>

### USB 诊断

需要排查设备发现或写入问题时，加上 `--verbose`：

```console
./build/ledscribe --verbose --list-devices
./build/ledscribe --verbose "洛汐"
```

诊断日志仅写入 stderr，不改变正常 stdout 输出或退出状态；默认不输出诊断日志。
日志包含 USB 总线与设备地址、十六进制 VID:PID、配置与接口/端点选择，
以及每个报告包的 OUT/ACK 阶段、传输字节数和 libusb 状态。
传输前会记录当前阶段，方便定位卡在发送还是等待 ACK；失败仍按原有错误处理退出。
可用 `2>usb.log` 保存诊断与错误信息；分享前请检查其中的设备标识。
`--dry-run` 不访问 USB，因此即使指定 `--verbose` 也不会产生 USB 诊断日志。

## 📄 许可说明

项目代码采用 [MIT License](./LICENSE)，保留原作者版权声明。项目由 [ProjectHazelita](https://github.com/ProjectHazelita) 维护，最初由 LYCN 和 LyCecilion 编写。

商家 `ASC11` / `HZK11` 字库来自安装包，**项目代码的许可证不自动覆盖字库**；未经授权，不应单独提交或再分发。轮廓字体也应遵守各自的许可证，并仅将受信任的本地 TTF/TTC 文件交给 stb_truetype 解析：它不为不可信字体提供安全解析保证。
