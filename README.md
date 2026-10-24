# BattTray · 无线鼠标电量托盘

**Battery** + **Tray**：一个 **68 KB** 的原生 Windows 托盘小程序，直接显示无线鼠标的剩余电量。

起点是雷柏（Rapoo）2.4G 鼠标——这类接收器不会被 Windows 登记成电池设备，系统设置里根本看不到电量。
本项目通过逆向厂商私有 HID 协议直接读取，并按「协议族」扩展到了罗技、雷蛇、华硕 ROG、
ATK / VXE / VGN / 迈从 / 飞智 等主流型号。纯 Win32 API，无运行时框架，单线程，空闲 CPU 为 0。

[![最新版本](https://img.shields.io/github/v/release/wuxiena/batt-tray?label=release&color=2ea44f)](https://github.com/wuxiena/batt-tray/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078d4)

## 下载

> ### [⬇ 下载最新版 `BattTray.exe`](https://github.com/wuxiena/batt-tray/releases/latest)
>
> 免安装、无需 .NET / Python / VC++ 运行库，下载后**双击即可运行**（Windows 10 / 11，约 68 KB）。
> 仓库内的 [`dist/BattTray.exe`](dist/BattTray.exe) 与 Release 里的文件完全一致。

- 想开机自动启动：右键托盘图标 → **开机自动启动**。
- 想开机不显示但保留数据：关掉「桌面浮窗」即可，托盘图标始终在。
- 首次运行若在任务栏看不到图标，点任务栏的 `^`（隐藏的图标）把它拖出来即可。

| 托盘图标（直接显示百分比） | 桌面浮窗（环形进度） |
| :---: | :---: |
| ![托盘图标](docs/tray-icon.png) | ![桌面浮窗](docs/preview.png) |

上图第一行是浅色任务栏、第二行是深色任务栏、第三行是鼠标休眠时的缓存读数。

## 特性

| 项目 | 数值 |
| --- | --- |
| 可执行文件 | 68 KB（静态链接，无运行时依赖） |
| 私有内存 | 约 3.4 MB |
| 工作集 | 约 15.3 MB（其余为 user32 / gdi32 等系统共享 DLL） |
| 空闲 CPU | 0（不轮询、不定时器） |
| 线程数 | 1 |
| 句柄 / GDI 对象 | 191 / 11，反复重扫描与导出诊断后不变 |

雷柏机型走**被动监听**：鼠标醒着时每 3 秒主动推一帧，程序用 overlapped `ReadFile` 常驻挂起，
`MsgWaitForMultipleObjects` 等事件，没有轮询循环，放着不动几乎不耗电。

## 使用

1. 从 [Releases](https://github.com/wuxiena/batt-tray/releases/latest) 下载 `BattTray.exe` 直接运行。
2. 托盘出现电量图标，悬停显示 `雷柏 Rapoo  电量 92%`。
3. 桌面右下角有一个圆形**浮窗**（环形进度 + 中心百分比），可拖动，位置会记住。
4. **左键单击**托盘图标 = 显示 / 隐藏浮窗。
5. **右键菜单**：桌面浮窗、开机自动启动、重新扫描设备、导出诊断信息、退出。

### 托盘图标

托盘实际只按 **16×16** 渲染，画一个电池外形再往里塞数字必然糊成一团。所以这里让数字当主角：
深色圆角徽章 + 3×5 点阵放大后的百分比，按图标尺寸自动挑选最大整数倍（16px 下为 2 倍），
保证笔画不糊、边缘不发虚。

细节：圆角用 4×4 子采样算覆盖率做抗锯齿（否则 16px 下四角是硬切的阶梯）；徽章上浅下深一道
很轻的渐变加顶部一点高光，避免整块死黑；两位数字的字距跟着缩放走，左右留白才能对称。

### 显示状态

| 显示 | 含义 |
| --- | --- |
| 亮色数字 | 正在实时接收（鼠标醒着，雷柏约每 3 秒一帧） |
| 变暗的数字 | 鼠标休眠或刚开机，显示的是上次读数，悬停提示会写「N 分钟前」 |
| `-` | 尚未读到过数据，动一下鼠标即可 |

颜色：绿 = >50%，橙 = 21–50%，红 = ≤20%（低电量时弹一次气泡提醒）。

## 兼容性

| 品牌 / 方案 | 识别条件 | 协议 | 状态 |
| --- | --- | --- | --- |
| 雷柏 Rapoo（VT 等） | `24AE` + `FF00/0002` | 7 字节被动上报 `BB B0 … <电量>` | ✅ 实机验证 |
| 罗技 Logitech | `046D` + `FF00` | HID++ 2.0 特性 `0x1004` / `0x1000` | ⚠️ 按公开规范实现，未实机 |
| 雷蛇 Razer | `1532` | 90 字节特性报告 `07/80` | ⚠️ 同上 |
| 华硕 ROG | `0B05` | 64 字节 `CC 12 07` | ⚠️ 同上 |
| Compx 方案：ATK / VXE / VGN / 达尔优 / G-Wolves / 兰族 / Pulsar / Zaopin / Ninjutso … | `3554` 等 + 厂商页 | 短报告 `08-04`（Nordic52）／长报告 `08-7D`（Nordic54） | ⚠️ 同上 |
| LEOBOG | `3554:FA09` | 20 字节 `13 CA` | ⚠️ 同上 |
| 迈从 MCHOSE 等 Realtek 方案 | `FF01` | `11` 命令 / `13` 状态 / `E2` 事件 | ⚠️ 同上 |
| 飞智 Flydigi | `37D7` + `FFA0` | 33 字节 `00 5A A5 01 02 03` | ⚠️ 同上 |
| 其它蓝牙 / 2.4G 鼠标 | Windows 电池类设备 | `IOCTL_BATTERY_QUERY_*` | ✅ 通用回退 |

只有雷柏协议是在本机实机抓包验证过的（`BB B0 02 72 06 01 5D` → 93%）。其余协议来自公开资料与
他人实测报文，开发机没有对应硬件，**无法验证**，因此按"尽力兼容"处理：任何一步解析失败都不会显示
可疑数字，而是继续往下匹配，最终回退到 Windows 电池或 `-`。

未列出的品牌请先尝试：设备管理器的「人体学输入设备」里能否看到厂商自定义集合；不行就走系统电池回退。

## 协议族说明

程序不按品牌写分支，而是按**协议族**实现，所以同族新型号通常无需改代码：

```
雷柏      FF00/0002   in=7     BB B0 02 72 06 01 5D          末字节 = 电量
Compx 52  厂商页      out 12~32 08 04 …                     [6] = 电量
Compx 54  厂商页      out 64~80 08 7D 72 02 00 01 07 01      rx[1]=0x72 && rx[5]=0x07 → rx[7]
罗技      FF00        out 7/20  10/11 dev feat func          特性 0x1004 → [4]=电量
雷蛇      特性 90/91  07 80 + XOR 校验                       [10]/255*100
ROG       64/64       CC 12 07                               [6] = 电量
LEOBOG    FF02        13 CA                                  [6] = 电量
Realtek   FF01        out 11 / in 13                         帧内 0xE2 标记后一字节
飞智      FFA0        00 5A A5 01 02 03                     低半字节档位 0..15
```

## 排错：导出诊断信息

右键菜单 → **导出诊断信息…**，会在 exe 同目录生成 `BattTray-diag.txt`（UTF-8）：

```
BattTray diagnostics
version : 1.2.0
provider: 雷柏 Rapoo
protocol: vid 24AE · FF00/0002 · 7 字节被动上报
reading : 92
reports : in=7 out=7 feature=0

HID vid=24AE pid=1849 page=FF00 usage=0002 in=7 out=7 feat=0
...
match RAPOO passive page=FF00 usage=0002 in=7
```

里面是本机全部 HID 集合的 VID/PID、UsagePage/Usage、报告长度，以及最终命中的分支。
新品牌适配基本看这份文件就够，可直接贴到 issue 里。

## 编译

```bat
build.cmd
```

不需要 Visual Studio，使用 [zig](https://ziglang.org/download/) 的 `zig cc -target x86_64-windows-gnu`
交叉编译，产物为 `dist\BattTray.exe`。

`build.cmd` 会优先使用仓库内 `toolchain\ziglang\zig.exe`，找不到则回退到 PATH 上的 `zig.exe`。
工具链体积较大，未纳入版本库（已在 `.gitignore` 中排除），需要时自行解压到该路径。

调试构建：

| 宏 | 作用 |
| --- | --- |
| `-DBATT_SIM` | 每 1.5 秒注入一帧真实报文，用于离线验证 UI |

## 目录结构

```
dist/BattTray.exe      预编译成品（与 Release 资产一致）
src/BattTray.c         界面与消息循环（托盘图标、圆形浮窗、菜单）
src/providers.c        各协议族的设备枚举与电量读取
src/providers.h        协议层对外接口
build.cmd              一键编译
docs/tray-icon.png     README 截图：托盘图标各状态
docs/preview.png       README 截图：桌面浮窗
tools/                 调试与逆向辅助
  shots.py               生成 docs/ 下的截图（浮窗实拍 + 图标状态图）
  bmp2png.py             BMP 转放大 PNG
  capture.py             屏幕截图（纯 ctypes，无第三方库）
  provtest.c             控制台验证协议层（不参与发布）
  icontest.c             用程序自身的绘制代码渲染图标各状态
  findwin.py             枚举本程序窗口位置
  make_icon.py           生成 .ico
  probe/probe_hid.py     枚举 HID 集合、capabilities、报告描述符
  probe/probe_overlapped.py 重叠读取原始上报帧
```

## 设置存储

`HKCU\Software\BattTray`：`Float`（浮窗开关）、`FloatX` / `FloatY`（位置）、`Level` / `Stamp`（上次读数缓存）。
开机自启写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，可随时在菜单里关闭。

## 已知限制

- 只有雷柏协议经过实机验证；其余品牌为按公开资料实现的"尽力兼容"，可能出现读数为空或不准。
- 充电状态多数协议读不出可靠字段，因此一般不显示「充电中」。
- 雷柏机型休眠时不产生上报，只能显示上次读数（会明确变暗提示）。
- Windows 11 默认把新图标收进「隐藏的图标」区，需要在任务栏设置里手动打开（等价于把图标拖出来）。
- exe 未内嵌图标资源（构建机上 `zig rc` 不可用）；托盘与浮窗图标为运行时 GDI 绘制，
  观感不受影响，仅资源管理器里显示为默认图标。

## 关于本项目

**这个项目的代码、README 和截图脚本都是 AI 写的。**

完整说明：作者提出需求、在真实硬件上测试并验收；代码、文档与调试脚本由 AI 编程助手（WorkBuddy）
在对话中生成，其中也包括协议层的分层设计、圆形浮窗的 4× 超采样绘制、以及托盘图标的点阵渲染。
实现过程中逆向出的协议结论、踩过的坑和修复（例如同一接收器上多个 `FF00` 集合导致的误匹配）
都记录在提交信息里，可逐条追溯。

因此请你带着这个背景使用它：

- 代码是 AI 生成后**在 Windows 11 + 雷柏接收器上实测过**的，但覆盖范围有限——只有雷柏分支经过真机验证；
- 本仓库的 `dist/BattTray.exe` 与 Release 资产都由 `build.cmd` 从仓库内源码直接编译，无额外步骤；
- 若你发现可疑实现，欢迎开 issue，附上「导出诊断信息」生成的 `BattTray-diag.txt` 会非常有用。

## 参考与致谢

- **[JD13703202/rapoo-vt3-battery](https://github.com/JD13703202/rapoo-vt3-battery)**（MIT）

  据此确认了雷柏鼠标的电量协议：7 字节输入报告、首字节 `0xBB`、**末字节为电量百分比**，
  以及 `UsagePage 0xFF00` 厂商集合的定位方式。本项目的雷柏分支即来自该结论。

- **[XAVIER0229/GearPower](https://github.com/XAVIER0229/GearPower)**（MIT）

  提供了雷蛇、罗技、ROG、Compx、迈从、飞智等多个品牌的**实机报文记录**，本项目据此实现了
  对应协议族的帧格式与字段偏移。

- **[Fan4Metal/ATK_tray](https://github.com/Fan4Metal/ATK_tray)**、**[ForceLightning/libatk-rs](https://github.com/ForceLightning/libatk-rs)**（GPL-3.0）

  ATK / Compx 方案的 `08-04`、`08-7D` 命令结构参考。本项目只从中提取了**协议常量与帧结构**这类
  事实性信息，未复制任何代码。

- **[openrazer/openrazer](https://github.com/openrazer/openrazer)**（GPL-2.0）

  雷蛇 `0x07 / 0x80` 命令类、89 字节 XOR 校验与 transaction id 约定。

说明范围：以上均为**协议事实**（报文格式、命令号、字节偏移）的参考。本项目的 HID 枚举、重叠 I/O
常驻监听、协议族调度、托盘图标与浮窗的 GDI 绘制、PNG 编码与截图工具，全部为独立实现，
没有复制任何参考项目的源码。

### 许可证选择

参考项目的许可证分两类：`rapoo-vt3-battery` 与 `GearPower` 为 **MIT**，`libatk-rs` 与 `openrazer`
为 **GPL**。本项目只引用了后者的事实性协议常量（不受版权保护），未使用其代码，因此不构成衍生作品，
最终选择与主要参考项目一致的 **MIT**：

- 与本项目自身的定位一致，无 copyleft（传染性）要求，可自由用于闭源或商业场景；
- 按 MIT 条款，在 [LICENSE](LICENSE) 中保留了 `rapoo-vt3-battery` 的版权声明。

## License

[MIT](LICENSE)

Copyright (c) 2026 wuxiena

协议常量部分衍生自 [JD13703202/rapoo-vt3-battery](https://github.com/JD13703202/rapoo-vt3-battery)，
Copyright (c) 2025 RapooVT3Battery Contributors，同样以 MIT 授权。
