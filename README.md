# linuxqq-wayland-fix

修复 Linux QQ 以 **Wayland** 运行时的屏幕分享、剪贴板和截图异常。

📺 一分钟视频介绍：[B 站](https://www.bilibili.com/video/BV__________)

>本项目接替 linuxqq-wayland-native-screenshare-fix 和 [linuxqq-clipsync](https://github.com/SHORiN-KiWATA/linuxqq-clipsync)。

## 安装

### Arch Linux（AUR）

```bash
paru -S linuxqq-wayland-fix-git
```

### Debian 12+ / Ubuntu 24.04+ / Fedora 43+ / Arch

从 [Releases](https://github.com/SHORiN-KiWATA/linuxqq-wayland-fix/releases) 下载对应的包：

```bash
sudo apt install ./linuxqq-wayland-fix_*debian12_amd64.deb     # Debian 12+
sudo apt install ./linuxqq-wayland-fix_*ubuntu24.04_amd64.deb  # Ubuntu 24.04+
sudo dnf install ./linuxqq-wayland-fix-*.fc43.x86_64.rpm         # Fedora 43+
sudo pacman -U ./linuxqq-wayland-fix-*.pkg.tar.zst              # Arch（需先装好 linuxqq）
```

QQ 本体需另外安装（[官方下载](https://im.qq.com/linuxqq/)）。

### 从源码

依赖：C 编译器、make、pkg-config、wayland-scanner，以及 glib2（gio）、libX11、libwayland-client 的开发文件；libpulse、libpipewire-0.3 的开发文件（只用头文件，运行时不依赖）。

```bash
make
sudo make install PREFIX=/usr
```

### NixOS（Flake）

本仓库自带 `flake.nix`（已同步上游 v0.2.6），提供 `packages`、`overlays`、`nixosModules` 三个出口。NixOS 用户推荐直接用模块：

```nix
{
  inputs.linuxqq-wayland-fix.url = "github:yigexuanmu/linuxqq-wayland-fix-nix";

  outputs = { self, nixpkgs, linuxqq-wayland-fix, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      modules = [
        linuxqq-wayland-fix.nixosModules.default
        {
          nixpkgs.config.allowUnfree = true; # pkgs.qq 是 unfree
          programs.linuxqq-wayland-fix.enable = true;
        }
      ];
    };
  };
}
```

`enable = true` 会装好修复包和 `pkgs.qq`，并把 QQ 的路径写进启动器（`QQ_WAYLAND_FIX_QQ`），之后从应用菜单打开「**QQ（Wayland修复版）**」即可。可用 `linuxqq-wayland-fix --doctor` 自检；`programs.linuxqq-wayland-fix.qq = null` 可以只装修复包、不装 QQ。

不用模块的话：

```bash
nix build github:yigexuanmu/linuxqq-wayland-fix-nix
nix develop   # 进开发环境后直接 make
```

> `pkgs.qq` 是 unfree，flake 只在 nixpkgs 允许 unfree 时才引用它（判断 `pkgs.config.allowUnfree`）。不允许时 `packages.default` 依然能构建，但启动器要自己去 PATH 里找 QQ（`linuxqq` 或 `qq`），或读你设置的 `QQ_WAYLAND_FIX_QQ`；这种情况下 `--doctor` 的「QQ 内部实现」一节会显示找不到目录——包不知道你的 QQ 装在哪，属正常。想让 `nix build` / `nix run` 也自动带上 QQ，把 `{ allowUnfree = true; }` 写进 `~/.config/nixpkgs/config.nix`（flake 的 nixpkgs 会读这个文件）。

> 打包时对上游启动器做了七处 NixOS 适配，都写在 `nix/package.nix` 里，不改仓库源码：三处脚本补丁（`compgen` 内建缺失、QQ 安装目录不在 `/opt/QQ`、nixpkgs 的 QQ 命令叫 `qq`）与四处运行环境——
>
> ① 把 `pipewire` 的库目录加进 `LD_LIBRARY_PATH`。QQ 的 `broadcast-core.so` 是用 `dlopen("libpipewire-0.3.so.0")` 取 PipeWire 的，Debian/Arch 的 `/usr/lib` 本来就在默认搜索路径里，NixOS 没有全局库目录、QQ 也不自带，不补这一步屏幕共享会走到采集前就失败（不弹 portal 选择框、对方看不到画面）。
>
> ② 在 Wayland 会话里设置 `EGL_PLATFORM=wayland`（已设置过则不动，非 Wayland 会话不设）。NixOS 的 glvnd 在没有任何平台提示时，会把 `eglGetDisplay(EGL_DEFAULT_DISPLAY)` 交给 Mesa 厂商应答，而 Mesa 驱动不了 NVIDIA 闭源驱动、只能退化成 llvmpipe 软件渲染：屏幕共享时插件进程里 12 个 `llvmpipe-*` 线程各占约 50%，合计约 5 个核（实测 484%~570%）。显式声明 Wayland 平台后，同一个调用由驱动的 EGL 应答，采集与格式转换走 GPU，同一种共享（2560×1600、`video format 8`）实测降到 22%~81%。（AMD/Intel 上 Mesa 能直接驱动硬件，这条基本是空操作。）
>
> ③ 补上硬件编解码库路径：把 NixOS 的图形驱动聚合目录 `/run/opengl-driver/lib` 追加进 `LD_LIBRARY_PATH`，并补上 `libva`。QQ 的 `broadcast-core.so` 与系统 ffmpeg 的 `libavcodec` 都是用**裸名** `dlopen` 这些库（`libavcodec` 自身没有 RUNPATH），Arch/Debian 的 `/usr/lib` 里本来就有，所以上游没管；NixOS 没有全局库目录，这些库默认一个都加载不到。`libva` 用 nixpkgs 的通用实现，它编译时就把驱动目录设成了 `/run/opengl-driver/lib/dri`，所以三家的 VA-API 驱动都能找到。
>
> **但要如实说明：这一条在 NVIDIA 上实测并没有换来硬件编码。** 补上之后，插件进程仍然映射 `libx264.so.165`（软件编码），`VideoEncode` 线程 62%~134%、长共享时还在 `DroppedFrame`；同一份 `LD_LIBRARY_PATH` 下 `libnvidia-encode.so.1`/`libcuda.so.1` 都能 `dlopen`、系统 ffmpeg 也带 `h264_nvenc`，但 QQ 根本没去映射它们——**用不用硬件编码是腾讯 AVSDK 自己探测决定的，补环境变量只能满足它的前置条件、不能替它做选择**。保留这个目录是为了厂商中立（AMD 的 VA-API/AMF、Intel 的 oneVPL 也从这里加载，未实测），代价只是多个兜底搜索路径。
>
> ④ 设置 `VK_DRIVER_FILES` 指向 `/run/opengl-driver/share/vulkan/icd.d/*.json`（各家 ICD 全带上）。上游启动器只查 `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`、`/usr/share/vulkan/icd.d`、`/etc/vulkan/icd.d` 与 `XDG_DATA_HOME` 下的 `icd.d`，在 NixOS 上永远探测不到 Vulkan，于是不会加 `--use-angle=vulkan`（Arch 上的默认行为，能避开部分设备上 Wayland + ANGLE 的 GLES 后端把共享画面渲染花的问题）。不想要就设 `QQ_WAYLAND_FIX_ANGLE=off`。
>
> 三家 GPU 的硬件编解码（第 ③ 条）分别要什么：
>
> | GPU | 硬件编解码接口 | 要额外装什么 |
> |---|---|---|
> | NVIDIA | NVENC / NVDEC（`libnvidia-encode.so`、`libnvcuvid.so`、`libcuda.so`） | 不用，驱动自带 |
> | AMD | AMF（`libamfrt64.so.1`）/ VA-API | `hardware.graphics.extraPackages = [ pkgs.amf ]`（unfree） |
> | Intel | oneVPL/QSV（`libmfx.so.1`）/ VA-API | `hardware.graphics.extraPackages = [ pkgs.vpl-gpu-rt pkgs.intel-media-driver ]` |
>
> 放进 `hardware.graphics.extraPackages` 的包会被 NixOS 聚合到 `/run/opengl-driver/lib`，而本包正好把这个目录交给了 QQ——所以上面这些装与不装，直接决定 QQ 能不能走硬件编解码。

## 注意事项

目前仅支持原生 linuxqq，不支持沙盒版本，KDE Plasma 桌面的支持也存在一些问题；

XWayaland需要正常工作；

屏幕共享需要桌面的 Portal 正常工作；

剪贴板需要桌面支持 data-control 协议。

## 使用方法

完全退出QQ（包括托盘），然后从应用菜单打开「**QQ（Wayland修复版）**」。

- 屏幕分享
  
  共享屏幕：在 QQ 自己的选窗里随便选「桌面」→「确定」，然后在合成器弹出的选择框里选真正要共享的屏幕或窗口；需要共享电脑声音时，点共享工具栏上的「共享设备音频」。共享时那个全屏的「屏幕共享」边框窗口会被自动隐藏，中间的「共享中」工具条不受影响。

- 剪贴板
  
  照常复制粘贴即可。

- 截图
  
  截图应该不再闪退。在平铺式合成器上截图窗口可能显示异常，见「已知问题」。

- 检查环境
  
    检查环境、以及 QQ 更新后修复是否仍然适用：

    ```bash
    linuxqq-wayland-fix --doctor
    ```

    QQ 崩溃时，崩溃记录（Bugly 的 `tomb_*.txt`）会保存到 `~/.cache/linuxqq-wayland-fix/crash/`（原位置会被 `linuxqq` 启动脚本清空），反馈问题时请附上。

## 工作原理

启动器通过 `LD_PRELOAD` 向 QQ 注入四个小库，不修改任何 QQ 文件。

**屏幕共享（`libqq-wl-portal.so`）**：QQ 的采集库 `broadcast-core.so` 其实自带一套 portal + PipeWire 的 Wayland 采集代码，但缺少「选择共享源」这一步，从未启用。本库只对 broadcast-core 发起的调用生效：让它走 Wayland 分支、在它连接 PipeWire 时自己走一遍 portal 选择流程，并修正两个 QQ 自身的 bug（声卡格式不是 s16le/f32le 时设备音频静默失败；共享内存帧忽略行跨度导致画面斜切）。另外，显示器坐标不从 0 开始时（如 Hyprland 单屏 `position=1920x0`），QQ 会给某个窗口算出空的几何并主动崩溃；本库在 QQ 主进程里把这处崩溃改为跳过该请求（#1，由 [@YoungJurry](https://github.com/YoungJurry) 最早定位）。

**剪贴板（`libqq-clipbridge.so`）**：QQ 的剪贴板代码（`wrapper.node` 里的 `ClipBoardHelper`）只用 Xlib，所以 QQ 在 Wayland 下只读写 X11 剪贴板。本库在 QQ 进程里起一个后台线程，用自己的 X 连接和 data-control 协议双向桥接：QQ 复制时把格式提供给 Wayland，别的程序复制时接管 X11 剪贴板；数据都在粘贴时按需传输一次。

**截图（`libqq-screenshot.so`）**：启动器为了让屏幕共享可用，给 QQ 的是 `XDG_SESSION_TYPE=x11`，于是 QQ 用 X11 的方式对根窗口 `XGetImage` 截全屏；而 Wayland 下的 XWayland 是 rootless 的，根窗口没有内容，这一步必然失败，QQ 不检查返回值就直接崩溃。本库拦截对根窗口的截取，改为通过 `wlr-screencopy` 截取各个 Wayland 输出，按 X 的显示器布局拼好交给 QQ；合成器不支持时给一张黑图，至少不再闪退。

**共享边框（`libqq-borderfix.so`）**：共享时显示的「屏幕共享」全屏边框窗口，是 QQ 原生代码通过 Chromium 建的 Wayland 窗口（Electron 的窗口 API 看不到；QQ 的 Chromium 又静态链接了自带的 Wayland client，符号注入也拦不到）。本库在 socket 层解析出站的 Wayland 线协议，只把这条窗口的 `attach(buffer)` 改写成 `attach(NULL)`，让它始终不映射出来；共享预览和「共享中」工具条不受影响。`QQ_BORDER_FIX_DISABLE=1` 可以关掉。

详细的逆向分析见 [docs/原理详解.md](docs/原理详解.md)。

## 已知问题

- 使用 Easy Effects 时，需在它的「输入」「输出」排除名单里都加上 `TRAE`，否则 QQ 一开通话/共享就会崩；
- 不要同时运行 linuxqq-clipsync 等其它剪贴板同步工具；
- 截图窗口在 niri 等平铺式合成器上会被平铺，画面重复显示；KDE、GNOME 下截图背景是黑的；
- 流畅度取决于 QQ 自己的编码；
- 观看别人共享时画面可能花成横竖条纹：Wayland 下 ANGLE 的 GLES 后端模拟 `GL_LUMINANCE` 纹理有问题，启动器检测到 Vulkan 时会自动用 `--use-angle=vulkan` 规避；没有 Vulkan 时可设 `QQ_WAYLAND_FIX_ANGLE=swiftshader`（较慢）。详见 [原理详解](docs/原理详解.md#附观看共享花屏)。

详细说明见 [常见问题与排错](docs/常见问题与排错.md)。

## 排错

日志：`$XDG_RUNTIME_DIR/linuxqq-wayland-fix.log`

```bash
grep -E 'qq-wl-portal|qq-clipbridge' "$XDG_RUNTIME_DIR/linuxqq-wayland-fix.log"
```

正常的输出：

```
[qq-clipbridge] ready (pid 12345, ext-data-control)
[qq-wl-portal] broadcast-core asked to connect fd=1, opening portal       ← 开始共享
[qq-wl-portal] portal ok: pipewire fd=82 node=127
[qq-wl-portal] device audio: report sample format 7 as float32le (5) …   ← 开启共享设备音频
[qq-clipbridge] QQ copied -> Wayland: text/plain;charset=utf-8 …          ← QQ 里复制
[qq-clipbridge] Wayland clipboard changed -> X11 for QQ: image/png        ← 别处复制
```

| 症状                                                                     | 原因 / 办法                                                                                                                                                     |
| ------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 提示「Wayland桌面环境暂时无法使用屏幕分享功能」                          | 不是从「QQ（Wayland修复版）」打开的                                                                                                                             |
| 点共享没反应，`coredumpctl` 有 QQ 的 SIGTRAP，栈里有 `PulseAudioWrapper` | Easy Effects，见上文                                                                                                                                            |
| 点「确定」没反应，栈里有 `ZSTD_` / `libgallium`                          | QQ 自带的 zstd 与 Mesa 冲突；启动器已设置 `MESA_SHADER_CACHE_DISABLE=true`，请确认没被覆盖                                                                      |
| 日志里没有任何 `qq-wl-portal` / `qq-clipbridge`                          | QQ 没被注入（旧 QQ 没退干净），或 QQ 更新后改了实现，运行 `--doctor`                                                                                            |
| `compositor supports neither …`                                          | 合成器不支持 data-control（如 GNOME），剪贴板修复不可用                                                                                                         |
| `response=1`                                                             | 在 portal 选择框里点了取消                                                                                                                                      |
| 按截图键 QQ 闪退，日志里有 `X_GetImage` 的 `BadMatch`                    | 截图修复没有生效（旧 QQ 没退干净，或没从「QQ（Wayland修复版）」打开）；正常时日志里有 `[qq-screenshot] captured …`                                              |
| 点「确定」开始共享时 QQ 闪退，崩溃记录里是 `signal: 5 (SIGTRAP)`         | 显示器坐标不从 0 开始时 QQ 算出空的窗口几何（#1）；本工具会自动处理，日志里应有 `empty-geometry fix: patched`，若是 `not patching` 说明 QQ 更新改了实现，请反馈 |
| 共享时仍出现全屏「屏幕共享」边框窗口                                      | 边框隐藏没生效：确认旧 QQ 已完全退出（含托盘）再从「QQ（Wayland修复版）」启动；正常时日志里应有 `hiding 屏幕共享 border window`，没有的话运行 `--doctor` 并反馈 |

排查时可以单独关掉某个修复：`QQ_WL_NATIVE_DISABLE=1`（屏幕共享）、`QQ_CLIPBOARD_FIX_DISABLE=1`（剪贴板）、`QQ_WL_GEOMETRY_FIX_DISABLE=1`（共享时防闪退）、`QQ_SCREENSHOT_FIX_DISABLE=1`（截图）、`QQ_BORDER_FIX_DISABLE=1`（共享边框隐藏）。

## 致谢

[littlekan233/qq-wayland-screenshare](https://github.com/littlekan233/qq-wayland-screenshare)、[xuwd1/wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare)：「截屏中转」思路的先行者。本项目采用了不同的方法，不包含它们的代码。

[@YoungJurry](https://github.com/YoungJurry) 定位了显示器坐标偏移时共享闪退的问题（#1、#2）。

## 许可证

MIT。`protocol/` 下的协议描述文件保留其原有版权声明。
