# LedScribe

Fedora/Linux 原生的 `0416:5020 / wch.cn CH546` 11×44 LED 显示屏写入工具，由 [ProjectHazelita](https://github.com/ProjectHazelita/LedScribe) 维护，原作者为 Limity'roChen 和 LyCecilion。

> To the ages long past, I bid my most devout farewell, even as the world doth bid farewell to me.

LedScribe 使用 C++20、libusb 与 stb_truetype。它支持商家 `ASC11`/`HZK11` 原版点阵字库，也能用普通 TTF/TTC 字体渲染；每个 64 字节 OUT 报告后都会读取 CH546 的 64 字节返回包并校验长度。

## 构建与测试

进入 Nix 开发环境后：

```console
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

程序位于 `build/ledscribe`。编译数据库位于 `build/compile_commands.json`，clangd 会自动读取。需要将所有编译器警告视为错误时，配置时加入 `-DLEDSCRIBE_WARNINGS_AS_ERRORS=ON`。

在允许 LeakSanitizer 检查进程的普通终端中运行完整 sanitizer 测试：

```console
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

该 preset 独立构建到 `build/sanitizers`，启用 ASan、UBSan 与 LSan；任何地址错误、未定义行为或内存泄漏都会令测试失败。

### 从旧名称迁移

项目原名 LEDyRochen，现在的仓库为 `ProjectHazelita/LedScribe`。命令、CMake 目标和 udev 规则统一采用 `ledscribe`，不保留旧命令别名；原作者版权声明保留。

如果重命名了源码目录，旧 CMake 缓存及 FetchContent 子构建仍可能保存旧绝对路径。关闭正在运行的构建后，可仅清理生成的依赖子构建并重新配置：

```console
cmake -E rm -rf build/_deps/stb-subbuild
cmake --preset dev --fresh
cmake --build --preset dev
```

若使用过 sanitizer 构建，需对 `build/sanitizers/_deps/stb-subbuild` 和 `sanitizers` preset 做相同处理。旧生成目录中可能仍有 `ledyrochen` 二进制，请使用新命令 `ledscribe`，不要误运行旧产物。

本地 Git 远程地址不会随网页上的仓库转移自动更新。需要时执行：

```console
git remote set-url origin git@github.com:ProjectHazelita/LedScribe.git
```

## USB 权限

首次使用时安装仅匹配这块显示屏的 udev 规则：

```bash
sudo install -Dm644 udev/70-ledscribe.rules /etc/udev/rules.d/70-ledscribe.rules
sudo udevadm control --reload-rules
```

然后拔下并重新插入显示屏。

如果之前手动安装过 `70-ledyrochen.rules`，请在新规则安装完成后移除系统中的旧同名规则，再重载。`uaccess` 规则面向活跃的本地桌面会话，不代表 SSH 或系统服务自动获得访问权限。

## 使用

列出设备：

```console
./build/ledscribe --list-devices
```

预览并只编码、不写入：

```console
./build/ledscribe --vendor-font-dir ./vendor --preview --dry-run "洛汐"
```

写入一至八条循环消息：

```console
./build/ledscribe --vendor-font-dir ./vendor "洛汐" "LUOXI OK" "你好，世界"
```

常用选项：

```text
--device BUS:DEV:OUT:IN
--speed 4              # 或 1,4,8，按槽位顺序设置
--brightness 25|50|75|100
--mode left|right|up|down|still|animation|drop|curtain|laser
--blink 1,3            # 或 all
--border all           # 或 1,3
--font /path/to/font.ttf
--font-size 11
--threshold 96
--vendor-font-dir /path/to/vendor
--no-vendor-font
```

`--speed`、`--mode` 的单个值作用于全部槽位；多个值按顺序设置槽位，未指定的槽位保留默认值。列表最多八项，空项和越界值会报错。数字模式的协议编码为：left=0、right=1、up=2、down=3、still=4、animation=5、drop=6、curtain=7、laser=8。

不提供 `--vendor-font-dir` 时，工具会尝试当前目录的 `vendor/ASC11` 与 `vendor/HZK11`。显式指定的目录必须同时包含这两个文件，否则报错，不再静默使用其他目录。使用 `--font` 或 `--no-vendor-font` 可跳过商家字库；这些路径下不会使用 `--vendor-font-dir`。

遇到无法用 GBK 编码的字符时，该条消息会整体回退到轮廓字体，终端会输出实际字体路径。若轮廓字体也缺少某个字符，程序会报告对应的 `U+XXXX` 并停止编码，而不是把缺字方框当作成功结果；请使用覆盖整条消息的 `--font`。字体大小必须大于 5，阈值必须在 0..255 范围内，即使本次使用商家字库也会检查参数。

`--preview` 展示完整的 11 行位图，并非 44 列设备视口或动画模拟；`--dry-run` 只渲染、编码，不访问 USB。普通轮廓字体渲染不保证复杂文字 shaping、双向排版或 emoji 序列排版正确。

仅使用受信任的本地 TTF/TTC 文件；stb_truetype 不为不可信字体提供安全解析保证。商家字库来自安装包，不应在没有授权的情况下单独提交或再分发，项目代码的许可证不自动覆盖字库。

写入后若仍显示充电画面，拔掉 USB 或按 USB 接口旁的按钮。

## 实现结构

- `src/vendor_font.*`：原版 ASC11/HZK11 加载、校验与点阵渲染
- `src/truetype.*`：UTF-8 轮廓字体渲染与二值化
- `src/pack.*`、`src/protocol.*`：位图封包、协议头与 64 字节补齐
- `src/device.*`：只移动的 RAII libusb 设备与 CH546 OUT/ACK 握手
- `tests/tests.cpp`：UTF-8、字库、封包和协议回归测试
- `tests/font_tests.cpp`：隔离临时目录中的字库发现与显式路径优先级测试
- `tests/cli.cmake`：使用合成字库的参数边界与多消息离线编码测试

离线测试不连接胸牌，也不依赖系统默认字体。显示模式编码已恢复为项目早期实现中的映射，并覆盖序列化回归；实际方向、动画效果、USB ACK 内容和必要发送间隔仍需真实设备确认。
