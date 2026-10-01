# linuxqq-wayland-screenshare-fix

让 Linux QQ 在 Wayland 下正常**共享屏幕**和**共享电脑声音**。

不搬运画面、不冒充 X11 截屏：QQ 其实自带一套基于 xdg-desktop-portal + PipeWire 的 Wayland 采集代码，只是缺了「选择共享源」这一步、一直没被启用。本项目补上这一步，让 QQ 自己的代码直接从合成器拿画面。

| | |
| --- | --- |
| 屏幕/窗口共享 | ✅ 由合成器（niri / KDE / GNOME / wlroots 系）弹出 portal 选择框 |
| 共享设备音频（电脑声音） | ✅ 修复了声卡为 s32le/s24le 时没有声音的问题 |
| 窗口共享画面错位 | ✅ 修复了 QQ 忽略行跨度、画面斜切成条纹的问题 |
| CPU 开销 | QQ 主进程几乎不增加（对比「截屏中转」类方案，主进程可少占约一个核心） |
| 改动 QQ 文件 | ❌ 不修改任何 QQ 文件，只通过 `LD_PRELOAD` 注入 |

> 实测环境：Arch Linux + niri + xwayland-satellite，QQ 3.2.34-53644。其它合成器理论上可用（标准 portal），欢迎反馈。

## 安装

### Arch Linux（AUR）

```bash
paru -S linuxqq-wayland-native-screenshare-fix-git
```

依赖 `linuxqq`，AUR 上提供它的包（`linuxqq`、`linuxqq-nt-bwrap`、`linuxqq-appimage` 等）任选其一。目前只在 `linuxqq` 上实测过。

### Debian 12+ / Ubuntu 24.04+ / Fedora 43+ / Arch

从 [Releases](https://github.com/SHORiN-KiWATA/linuxqq-wayland-screenshare-fix/releases) 下载对应发行版的包：

```bash
sudo apt install ./linuxqq-wayland-native-screenshare-fix_*debian12_amd64.deb     # Debian 12+
sudo apt install ./linuxqq-wayland-native-screenshare-fix_*ubuntu24.04_amd64.deb  # Ubuntu 24.04+
sudo dnf install ./linuxqq-wayland-native-screenshare-fix-*.fc43.x86_64.rpm         # Fedora 43+
sudo pacman -U ./linuxqq-wayland-native-screenshare-fix-*.pkg.tar.zst              # Arch（需先装好 linuxqq）
```

QQ 本体需要另外安装（[官方下载](https://im.qq.com/linuxqq/)，Arch 用 AUR 的 `linuxqq`）。

### 从源码

依赖：C 编译器、make、pkg-config、glib2（gio）开发文件；libpulse 与 libpipewire-0.3 的开发文件（只用头文件，运行时不依赖）。

```bash
make
sudo make install PREFIX=/usr
```

## 使用

1. **完全退出 QQ**（包括托盘）。
2. 从应用菜单启动「**QQ（屏幕共享修复）**」，或在终端运行 `linuxqq-wayland-native-screenshare-fix`。
3. 发起共享 → QQ 自己的选窗里随便选「桌面1」→ 点「确定」。
4. 合成器弹出 portal 选择框，在**这里**选真正要共享的屏幕或窗口。
5. 需要共享电脑声音时，点共享工具栏上的「共享设备音频」。

同时安装了 [linuxqq-wayland-clipboard-fix](https://github.com/SHORiN-KiWATA/linuxqq-wayland-clipboard-fix)（修复 Wayland 下的剪贴板）时，从任意一个入口打开，两个修复都会生效。

QQ 崩溃时，崩溃记录（Bugly 的 `tomb_*.txt`）会被保存到 `~/.cache/linuxqq-wayland-native-screenshare-fix/crash/`（原位置会被 `linuxqq` 启动脚本清空），反馈问题时请附上。

检查环境与当前 QQ 版本是否兼容：

```bash
linuxqq-wayland-native-screenshare-fix --doctor
```

## 必读：已知问题与设置

### 使用 Easy Effects 时，QQ 一开通话/共享就崩

Easy Effects 会把新出现的音频流移到它自己的设备上，这会触发 QQ 音频模块里的竞态 bug（采集进程 SIGTRAP）。解决：

- Easy Effects →「输入」和「输出」页 → 排除的应用 → 都加上 **`TRAE`**（QQ 音频流的应用名）；
- QQ「设置 → 音视频通话」里把麦克风选成 **Easy Effects Source**，麦克风照样经过 Easy Effects 处理。

`linuxqq-wayland-native-screenshare-fix --doctor` 会检查这一项。

### niri 上的 Linux QQ 看别人的共享画面花屏

在 niri 上用 Linux QQ 3.2.34 **观看**共享时，画面可能缩在一角、满是竖条纹；同一路共享在手机 QQ、Windows QQ 上看是正常的。不注入本项目的原版 QQ 也一样，有用户反馈 niri + QQ 3.2.32 观看正常，推测是 QQ 3.2.34 接收端的问题，与本项目和发送端无关。

### 全屏蓝色边框

QQ 为 X11 设计的共享边框在 Wayland 下会变成一个真实的全屏窗口。目前没有处理，可以在合成器里把它挪到别的工作区。

### 流畅度

画面进入 QQ 之后由 QQ 单线程软件 H.264 编码，帧率由 QQ 自己的策略决定，本项目无法改善。想更流畅，在 portal 选择框里选**单个窗口**而不是整个屏幕。

### 仍需要 XWayland

QQ 的界面流程仍以为自己在 X11 会话中（选窗缩略图等），所以需要 XWayland（`DISPLAY` 不能为空）。画面采集本身完全走 Wayland。

## 排错

日志：`$XDG_RUNTIME_DIR/linuxqq-wayland-native-screenshare-fix.log`

```bash
grep qq-wl-portal "$XDG_RUNTIME_DIR/linuxqq-wayland-native-screenshare-fix.log"
```

正常的输出：

```
broadcast-core asked to connect fd=1, opening portal
portal ok: pipewire fd=82 node=127
stream connect target 0 -> 127
stream format 1235x812 (video format 8)
repacking frames: 1235x812 stride 4992 -> 4940 offset 0                    ← 帧有行填充时
device audio: report sample format 7 as float32le (5) to broadcast-core   ← 开启共享设备音频时
```

| 症状 | 原因 / 办法 |
| --- | --- |
| 提示「Wayland桌面环境暂时无法使用屏幕分享功能」 | 没有用 `linuxqq-wayland-native-screenshare-fix` 启动 |
| 点共享没反应，`coredumpctl` 有 QQ 的 SIGTRAP，栈里有 `PulseAudioWrapper` | Easy Effects，见上文 |
| 点「确定」没反应，栈里有 `ZSTD_` / `libgallium` | QQ 自带的 zstd 与 Mesa 冲突；启动脚本已设置 `MESA_SHADER_CACHE_DISABLE=true`，请确认没被覆盖 |
| 日志里一行 `qq-wl-portal` 都没有 | QQ 没被注入（旧 QQ 没退干净），或 QQ 更新后改了实现，运行 `--doctor` |
| `response=1` | 在 portal 选择框里点了取消 |
| 想确认是不是本项目的问题 | `QQ_WL_NATIVE_DISABLE=1 linuxqq-wayland-native-screenshare-fix`：注入但不做任何拦截 |

## 工作原理

QQ 的屏幕采集在音视频插件进程（`--type=ppapi`）里的 `broadcast-core.so` 中，它有两条路径：

```c
if (IsWayland())   // getenv("XDG_SESSION_TYPE") == "wayland"
    // 句柄低 32 位当 PipeWire fd、高 32 位当 node id，连 portal 给的流
    MonitorCapture_WaylandProc();
else
    MonitorCapture_X11Proc();      // XShmGetImage 截 X 根窗口
```

Wayland 分支需要的 fd 和 node id 本该由 QQ 的 `CaptureSelector`（调用 portal 的选择器）提供，但上层从不调用它，界面在 Wayland 下也直接拦截了共享。本项目的注入库（`libqq-wl-portal.so`）做了五件事，**全部只对 `broadcast-core.so` 发起的调用生效**（按返回地址判断调用方），QQ 其余部分不受影响：

1. **`getenv("XDG_SESSION_TYPE")`**：对 broadcast-core 返回 `wayland`，让它走 Wayland 分支；QQ 其它部分看到的是启动脚本设置的 `x11`，所以界面不拦截。
2. **`dlsym`**：broadcast-core 用 `dlopen` + `dlsym` 获取 PipeWire 函数。取 `pw_context_connect_fd` / `pw_stream_connect` / `pw_core_disconnect` 时返回包装函数。其它 `dlsym` 调用通过汇编蹦床**尾跳转**给真 dlsym，保证 glibc 看到的调用者不变（`RTLD_NEXT` 依赖它）。
3. **选择器**：包装函数里自己走一遍 portal ScreenCast（CreateSession → SelectSources → Start → OpenPipeWireRemote），把真正的 PipeWire fd 和 node id 换进去；结束时关闭会话。
4. **`pa_context_get_{sink,source}_info_by_name`**：broadcast-core 的设备音频采集只接受 S16LE / FLOAT32LE 格式的输出设备，否则返回 `E_NOTIMPL`（静默失败）。这里把格式改报为 FLOAT32LE，实际格式转换由 PipeWire 完成。
5. **修正行跨度**：QQ 对共享内存帧直接 `memcpy(width*height*4)`，忽略了 `chunk->stride` / `offset`。合成器给的每行带填充时（共享宽度不规整的窗口时很常见），画面会斜切成条纹。本库拦截 `pw_stream_add_listener` / `dequeue_buffer` / `queue_buffer`，记下协商的宽高，遇到带填充的帧就先重排成紧凑的一块再交给 QQ。

启动脚本还设置了 `MESA_SHADER_CACHE_DISABLE=true`：QQ 的 `libAVSDKPlugin.so` 导出了自带的 `ZSTD_*` 符号，会劫持 Mesa 着色器缓存的 zstd 解压，导致采集进程崩溃。

更详细的逆向分析见 [docs/原理详解.md](docs/原理详解.md)。

## QQ 更新后

QQ 会自动热更新到 `~/.config/QQ/versions/`。本项目不依赖任何地址或偏移，只依赖：broadcast-core 用 `XDG_SESSION_TYPE` 判断 Wayland、按名字 `dlsym` 获取 PipeWire 函数、用 `pa_context_get_sink_info_by_name` 查设备格式。此外依赖 broadcast-core 按名字获取 `pw_stream_add_listener` / `dequeue_buffer` / `queue_buffer`。更新后如果出问题，先运行 `linuxqq-wayland-native-screenshare-fix --doctor`，它会逐项检查这些前提。

## 致谢

- [littlekan233/qq-wayland-screenshare](https://github.com/littlekan233/qq-wayland-screenshare)、[xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare)：「截屏中转」思路的先行者。本项目采用了不同的方法（启用 QQ 自带的 Wayland 路径），不包含它们的代码。

## 许可证

MIT
