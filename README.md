# BattTray · 无线设备电量托盘

**Battery** + **Tray**：一个 **88 KB** 的原生 Windows 托盘小程序，把无线鼠标、键盘、耳机、手柄的
剩余电量直接画在任务栏上——**一台设备一个图标**，悬停就能看清品牌和百分比。

起点是雷柏（Rapoo）2.4G 鼠标——这类接收器不会被 Windows 登记成电池设备，系统设置里根本看不到电量。
本项目通过逆向厂商私有 HID 协议直接读取，并按「协议族」扩展到了罗技、雷蛇、华硕 ROG、
ATK / VXE / VGN / 迈从 / 飞智、索尼 DualSense、Xbox 手柄以及通用蓝牙耳机。
纯 Win32 API，无运行时框架，单线程，空闲 CPU 为 0。

[![最新版本](https://img.shields.io/github/v/release/wuxiena/batt-tray?label=release&color=2ea44f)](https://github.com/wuxiena/batt-tray/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078d4)

## 下载

> ### [⬇ 下载最新版 `BattTray.exe`](https://github.com/wuxiena/batt-tray/releases/latest)
>
> 免安装、无需 .NET / Python / VC++ 运行库，下载后**双击即可运行**（Windows 10 / 11，约 88 KB）。
> 仓库内的 [`dist/BattTray.exe`](dist/BattTray.exe) 与 Release 里的文件完全一致。

- 想开机自动启动：右键托盘图标 → **开机自动启动**。
- 想开机不显示但保留数据：关掉「桌面浮窗」即可，托盘图标始终在。
- 首次运行若在任务栏看不到图标，点任务栏的 `^`（隐藏的图标）把它拖出来即可，详见[常见问题](#任务栏里看不到图标)。

| 托盘图标：直接显示百分比（浅色 / 深色任务栏、休眠缓存） | 桌面浮窗：环形进度 + 中心百分比 |
| :---: | :---: |
| ![托盘图标](docs/tray-icon.png) | ![桌面浮窗](docs/preview.png) |

图标按 **16×16** 实拍渲染：第一行浅色任务栏、第二行深色任务栏、第三行是设备休眠时的缓存读数（变暗）。

一台设备一个托盘图标；**读不到电量的设备不占托盘**，只在右键列表里注明（见[多设备](#多设备)）。

## 特性

| 项目 | 数值 |
| --- | --- |
| 可执行文件 | 88 KB（静态链接，无运行时依赖） |
| 私有内存 | 约 3.8 MB |
| 工作集 | 约 16 MB（其余为 user32 / gdi32 等系统共享 DLL） |
| 空闲 CPU | 0（45 秒采样 0 ms，不轮询、不定时器） |
| 线程数 | 1 |
| 句柄 | 约 237，连续重扫描 5 次后不变 |

雷柏机型走**被动监听**：鼠标醒着时每 3 秒主动推一帧，程序用 overlapped `ReadFile` 常驻挂起，
`MsgWaitForMultipleObjects` 等事件，没有轮询循环，放着不动几乎不耗电。耳机 dongle 这类只有
「问一句答一句」的设备才会主动查询，且默认 20 秒一次；连续不应答会自动退避到约 3 分钟一次，
不会为了一个读不到的设备反复打扰 USB。

## 使用

1. 从 [Releases](https://github.com/wuxiena/batt-tray/releases/latest) 下载 `BattTray.exe` 直接运行。
2. 能读到电量的设备各占一个托盘图标，悬停显示 `雷柏 Rapoo 鼠标 · 92%`。
3. 桌面右下角有一个圆形**浮窗**（环形进度 + 中心百分比），可拖动，位置会记住。
4. **左键单击**托盘图标 = 显示 / 隐藏浮窗。
5. **右键菜单**：设备列表、桌面浮窗、开机自动启动、重新扫描设备、导出诊断信息、退出。

### 多设备

- 托盘图标按 **设备** 生成，不是按品牌：插两个能读电量的设备 = 两个图标。
- **读不到电量的设备不进托盘**，只在右键菜单里列出来并注明「电量不可读」，避免任务栏挂一堆
  没有信息的图标；一旦它开始应答，图标会自动补上。
- 一台都读不到时会保留一个图标当入口，否则菜单就再也打不开了。
- 一个接收器上挂多个 HID 集合（雷柏接收器就同时暴露 3 个）只会算**一台**设备，不会重复出图标。
- 右键菜单第一段是设备列表：勾选的是浮窗正在显示的那台，灰色的表示读不到电量、不可选。
- 悬停提示最前面是品牌，所以一排放着也能一眼分清是哪个设备。
- 桌面浮窗底部的小圆点是设备指示：亮着的那颗就是浮窗当前显示的设备；
  同一时刻浮窗只显示一台，多台都在托盘里各占一个图标。
- 识别顺序按「手柄 > 鼠标 > 键盘 > 耳机」定类别，同一接收器上报多种用途时不会认错。

### 托盘图标

托盘实际只按 **16×16** 渲染，画一个电池外形再往里塞数字必然糊成一团。所以这里让数字当主角：
深色圆角徽章 + 3×5 点阵放大后的百分比，按图标尺寸自动挑选最大整数倍（16px 下为 2 倍），
保证笔画不糊、边缘不发虚。

细节：圆角用 4×4 子采样算覆盖率做抗锯齿（否则 16px 下四角是硬切的阶梯）；徽章上浅下深一道
很轻的渐变加顶部一点高光，避免整块死黑；两位数字的字距跟着缩放走，左右留白才能对称。
设备多于一台时图标整体缩小一档、数字位宽左移，保证在较小的托盘格里仍然清晰。

### 显示状态

| 显示 | 含义 |
| --- | --- |
| 亮色数字 | 正在实时接收（鼠标醒着，雷柏约每 3 秒一帧） |
| 变暗的数字 | 设备休眠或已断开，显示的是上次读数，悬停提示会写「N 分钟前」 |
| `-` | 当前读不到电量：这台设备**不会出现在托盘**，只在右键菜单里标注（仅当一台都读不到时才用 `-` 占位当入口） |

颜色：绿 = >50%，橙 = 21–50%，红 = ≤20%（低电量时弹一次气泡提醒）。

## 兼容性

### 鼠标 / 键盘

| 品牌 / 方案 | 识别条件 | 协议 | 状态 |
| --- | --- | --- | --- |
| 雷柏 Rapoo（VT 等） | `24AE` + `FF00/0002` | 7 字节被动上报 `BB B0 … <电量>` | ✅ 实机验证 |
| 罗技 Logitech | `046D` + `FF00` | HID++ 2.0 特性 `0x1004` / `0x1000` | ⚠️ 按公开规范实现，未实机 |
| 雷蛇 Razer | `1532` | 90 字节特性报告 `07/80` | ✅ 实机验证 |
| 华硕 ROG | `0B05` | 64 字节 `CC 12 07` | ⚠️ 同上 |
| Compx 方案：ATK / VXE / VGN / 达尔优 / G-Wolves / 兰族 / Pulsar / Zaopin / Ninjutso … | `3554` 等 + 厂商页 | 短报告 `08-04`（Nordic52）／长报告 `08-7D`（Nordic54） | ⚠️ 同上 |
| LEOBOG | `3554:FA09` | 20 字节 `13 CA` | ⚠️ 同上 |
| 迈从 MCHOSE 等 Realtek 方案 | `FF01` | `11` 命令 / `13` 状态 / `E2` 事件 | ⚠️ 同上 |
| 飞智 Flydigi | `37D7` + `FFA0` | 33 字节 `00 5A A5 01 02 03` | ⚠️ 同上 |

### 耳机 / 手柄

| 品牌 / 方案 | 识别条件 | 协议 | 状态 |
| --- | --- | --- | --- |
| 0xFF90 厂商通道耳机 dongle（MCHOSE V9 Pro 同族） | 音频集合 + `FF90/0001` + 64 字节收发 | `55 65 01 00` → `55 65 <电量> <状态>` | ⚠️ 协议有公开逆向记录，未实机 |
| 索尼 DualShock 4 / DualSense | `054C` + `01/04`、`01/05` | 输入报文 `0x1E`（DS4）/ `0x36`（DualSense） | ⚠️ 同上 |
| Xbox 及 XInput 兼容手柄 | `XInputGetBatteryInformation` | 系统 API，4 档 → 5/25/60/100% | ✅ 通用回退 |
| 天剑PLUS / Leadjoy Xeno Plus（`4131` LiteEMF 方案） | `4131:3519` + XInput | XInput 谎报「有线 / 满电」，厂商通道无电量命令 | ⚠️ 实机确认：读不到 |
| 蓝牙耳机（AirPods、索尼 WH/WF、Bose QC 等） | Windows 电池类设备 | `IOCTL_BATTERY_QUERY_*` | ✅ 通用回退 |
| 其它 2.4G 耳机 | Windows 电池类设备 | 同上 | ✅ 通用回退 |

### 关于「为什么我的耳机不显示电量」

不是所有耳机 dongle 都会把电量交给电脑。**西伯利亚 XIBERIA K03S**（`10D6:4801`，Actions 方案）就是
典型例子：本机实测它确实暴露了 `FF90/0001` 厂商集合，也接受了 `55 65 01` 这条读电量指令（换成
`55 65 02` 会被直接拒绝），但**不论怎么等都不回话**，厂商集合全程一个字节都不主动推。这台耳机的
电量只显示在自己的屏上（或双击麦克风键触发屏显），PC 侧大概只有官方 X HUB 能读。

遇到这种情况，BattTray 的处理是：**不给它占托盘图标**，只在右键菜单的设备列表里列一行
「西伯利亚 XIBERIA 耳机 · 电量不可读（dongle 不提供）」。既不占任务栏位置，也不会编一个假数字给你。

### 关于「为什么我的手柄不显示电量」

**天剑PLUS（Leadjoy Xeno Plus）** 是实机查过的一个完整例子。它插上接收器后在系统里叫
`Controller (XBOX 360 For Windows)`，`4131:3519`，厂商串是 `LiteEMF`——一个开源的嵌入式中间件框架，
不少国产外设的固件都基于它。这台设备的接口摊开是这样：

| 集合 | 用途 | 报文 |
| --- | --- | --- |
| `0001/0005` | 手柄本体 | 15 字节输入（固定尾部 3 字节恒为 0） |
| `000C/0001` | 多媒体键 | 5 字节输入 |
| `0002/0001`、`0001/0002` | 键盘、鼠标（魔法键 / 体感映射鼠标） | 常规 |
| `FF80/0001` | LiteEMF 厂商通道 | 64 字节收发 |

两条路都不给电量：

1. **XInput 说谎。** `XInputGetBatteryInformation` 返回 `batteryType=1 (WIRED)`、
   `batteryLevel=3 (FULL)`——明明是 2.4G 无线连接，系统却当成插着线的满电手柄。这是第三方 dongle
   的常见做法：不想实现 Xbox 无线电池协议，就统一报「有线」，于是 Windows 设置里永远看不到电量。
2. **厂商通道没有电量命令。** `FF80` 说的是 LiteEMF 的 commander 协议，帧格式为
   `A5 <len> <cmd> <data…> <sum>`（`sum` = 前面所有字节之和取低 8 位）。把这套命令字
   `0x00`–`0xFE` 全扫了一遍（跳过会改设备状态的几条），手柄在线、离线各扫一次，能应答的只有：

   | 命令 | 含义 | 应答 |
   | --- | --- | --- |
   | `0x01` | 协议版本 | `01` |
   | `0x02` | UUID | 16 字节 |
   | `0x03` | 固件版本 | `00 20 00 00` |
   | `0x04` | 型号 | `TJ10_DG` |
   | `0x05` | MTU | `08 40` |
   | `0x0C` | 设备名 | 空 |
   | `0x10` | 工作模式 | 10 字节 |
   | `0x1E` | 心跳 | `00` |
   | `0xE2` | 设备信息 | `20 00 00` + `TJ10_DG` |
   | `0x1A` | 关机 / 断开 | `00`（之后设备会重新枚举，别碰） |

   **没有任何一条返回电量。** 电量只体现在手柄自己的 LOGO 灯上（红闪=低电、红色呼吸=充电中、
   充满熄灭），以及充电底座的四颗指示灯。官方「动感指尖」App 里能看电量，但那是手机直连手柄的
   路径，PC 上的 dongle 不转发这个数据。

所以对这类手柄，BattTray 的处理和耳机一致：**不占托盘图标**，只在右键菜单里列一行
「Xbox / XInput 手柄 · 电量不可读（手柄不提供）」，理由写在里面，不编数字。

### 通用说明

雷柏、雷蛇、西伯利亚、天剑PLUS 四台是在本机实机验证过的（雷柏实测 `BB B0 02 72 06 01 5D` → 93%，
雷蛇走 90 字节特性报告，另外两台是「确认读不到」）。其余协议来自公开资料与其他人实测的报文，
开发机没有对应硬件，**无法验证**，因此按「尽力兼容」处理：任何一步解析
失败都不会显示可疑数字，而是继续往下匹配，最终回退到 Windows 电池或 `-`。

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
耳机 0xFF90 FF90 64/64 55 65 01 00                           [2] = 电量（0..100）
索尼手柄  输入报文    0x1E / 0x36                           高/低半字节档位 0..10 → ×10
XInput    系统 API    XInputGetBatteryInformation            档位 → 5/25/60/100
```

品牌名分两层：主表按 VID 认（`24AE` → 雷柏…）；耳机 dongle 大多用芯片厂的 VID（Actions `10D6`、
C-Media `291D`），光看 VID 只会得到「Actions 方案」，所以这类设备会**优先读 HID 产品名**再映射
（`XIBERIA` → 西伯利亚、`EDIFIER` → 漫步者、`MCHOSE` → 迈从…），认不出就整串产品名当品牌用。

## 排错：导出诊断信息

右键菜单 → **导出诊断信息…**，会在 exe 同目录生成 `BattTray-diag.txt`（UTF-8）：

```
BattTray diagnostics
version  : 1.3.0
devices  : 2

[0] 雷柏 Rapoo / 鼠标
    protocol: 雷柏被动上报 · FF00/0002 · 7 字节
    vid=24AE pid=1849  reports: in=7 out=7 feature=0
    level=92  passive=1  xinput=0

[1] 西伯利亚 XIBERIA / 耳机
    protocol: 0xFF90 厂商通道 · 64 字节 · 无应答
    vid=10D6 pid=4801  reports: in=64 out=64 feature=0
    level=-1  passive=0  xinput=0

HID vid=24AE pid=1849 page=FF00 usage=0002 in=7 out=7 feat=0
...
match RAPOO page=FF00 usage=0002 in=7
match VEND90 answered=0 level=-1
```

里面是本机全部 HID 集合的 VID/PID、UsagePage/Usage、报告长度，以及每台设备最终命中的分支。
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
| `-DBATT_FAKE` | 注入 4 台假设备（鼠标/键盘/耳机/手柄），验证多图标与多设备布局 |

> `-DBATT_FAKE` 会把假设备的名称和电量写进 `HKCU\Software\BattTray` 的缓存槽位。如果之后插上同名的
> 真设备，它会先显示那个假读数——调试完记得清掉 `N1..N5` / `L1..L5` / `S1..S5`。

## 目录结构

```
dist/BattTray.exe      预编译成品（与 Release 资产一致）
src/BattTray.c         界面与消息循环（托盘图标、圆形浮窗、菜单）
src/providers.c        各协议族的设备枚举与电量读取
src/providers.h        协议层对外接口
build.cmd              一键编译
docs/tray-icon.png     README 截图：托盘图标各状态
docs/multi-device.png  README 截图：多设备托盘（实拍）
docs/preview.png       README 截图：桌面浮窗
tools/                 调试与逆向辅助
  shots.py               生成 docs/ 下的截图（浮窗实拍 + 图标状态图）
  bmp2png.py             BMP 转放大 PNG
  capture.py             屏幕截图（纯 ctypes，无第三方库）
  provtest.c             控制台验证协议层（不参与发布）
  icontest.c             用程序自身的绘制代码渲染图标各状态
  findwin.py             枚举本程序窗口位置
  make_icon.py           生成 .ico
  probe/dump_hid.py      枚举全部 HID 集合：VID/PID、UsagePage、报告长度
  probe/listen_hid.py    被动监听集合的上报帧（先听再发，避免把 dongle 弄掉线）
  probe/hid_xact.py      向厂商集合发帧并打印应答（逆向用）
  probe/vend_cmd.py      按 LiteEMF commander 帧格式收发，支持命令区扫描
  probe/probe_reportid.py 用 preparsed data 读出集合真正使用的 report id
  probe/probe_overlapped.py 重叠读取原始上报帧
```

## 设置存储

`HKCU\Software\BattTray`：`Float`（浮窗开关）、`FloatX` / `FloatY`（位置），以及每台设备的读数缓存
`N<i>` / `L<i>` / `S<i>`（设备名 / 电量 / 时间戳，按名字校验，避免重扫描后串台）。
开机自启写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，可随时在菜单里关闭。

## 常见问题

### 任务栏里看不到图标

Windows 11 默认把新出现的托盘图标收进「隐藏的图标」区（`^`）。BattTray 启动时会尽力把自己
「提升」为常显，但系统对这个状态的生效时机不完全可控，所以：

1. 点任务栏的 `^`，把 BattTray 的图标拖到外面即可；
2. 也可以在 设置 → 个性化 → 任务栏 → 其它系统托盘图标 里打开。

注意：常显状态是按「exe 路径 + 图标 ID」记录的，所以**换目录、改文件名、或程序升级后图标 ID 变了**，
都可能需要重新拖一次。

## 已知限制

- 只有雷柏协议和 Windows 电池回退经过实机验证；其余品牌为按公开资料实现的「尽力兼容」。
- 部分耳机 dongle（如西伯利亚 K03S）根本不提供电量通道，只能显示 `-`，见上文。
- 充电状态多数协议读不出可靠字段，因此一般不显示「充电中」（0xFF90 通道的状态字节含义未公开，故意忽略）。
- 设备休眠时不产生上报，只能显示上次读数（会明确变暗提示）。
- exe 未内嵌图标资源（构建机上 `zig rc` 不可用）；托盘与浮窗图标为运行时 GDI 绘制，
  观感不受影响，仅资源管理器里显示为默认图标。

## 关于本项目

**这个项目的代码、README 和截图脚本都是 AI 写的。**

完整说明：作者提出需求、在真实硬件上测试并验收；代码、文档与调试脚本由 AI 编程助手（WorkBuddy）
在对话中生成，其中也包括协议层的分层设计、圆形浮窗的 4× 超采样绘制、以及托盘图标的点阵渲染。
实现过程中逆向出的协议结论、踩过的坑和修复（例如同一接收器上多个 `FF00` 集合导致的误匹配、
以及某些 dongle 收到非法帧会自行重连）都记录在提交信息里，可逐条追溯。

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

- **[rafagfran/mchose-v9-pro-battery-tray](https://github.com/rafagfran/mchose-v9-pro-battery-tray)**（MIT）

  耳机 `0xFF90/0x0001` 厂商通道的 `55 65 01 00` 请求帧与 `55 65 <电量> <状态>` 应答格式。

- **[Fan4Metal/ATK_tray](https://github.com/Fan4Metal/ATK_tray)**、**[ForceLightning/libatk-rs](https://github.com/ForceLightning/libatk-rs)**（GPL-3.0）

  ATK / Compx 方案的 `08-04`、`08-7D` 命令结构参考。本项目只从中提取了**协议常量与帧结构**这类
  事实性信息，未复制任何代码。

- **[openrazer/openrazer](https://github.com/openrazer/openrazer)**（GPL-2.0）

  雷蛇 `0x07 / 0x80` 命令类、89 字节 XOR 校验与 transaction id 约定。

说明范围：以上均为**协议事实**（报文格式、命令号、字节偏移）的参考。本项目的 HID 枚举、重叠 I/O
常驻监听、协议族调度、托盘图标与浮窗的 GDI 绘制、PNG 编码与截图工具，全部为独立实现，
没有复制任何参考项目的源码。

### 许可证选择

参考项目的许可证分两类：`rapoo-vt3-battery`、`GearPower`、`mchose-v9-pro-battery-tray` 为 **MIT**，
`libatk-rs` 与 `openrazer` 为 **GPL**。本项目只引用了后者的事实性协议常量（不受版权保护），未使用其
代码，因此不构成衍生作品，最终选择与主要参考项目一致的 **MIT**：

- 与本项目自身的定位一致，无 copyleft（传染性）要求，可自由用于闭源或商业场景；
- 按 MIT 条款，在 [LICENSE](LICENSE) 中保留了 `rapoo-vt3-battery` 的版权声明。

## License

[MIT](LICENSE)

Copyright (c) 2026 wuxiena

协议常量部分衍生自 [JD13703202/rapoo-vt3-battery](https://github.com/JD13703202/rapoo-vt3-battery)，
Copyright (c) 2025 RapooVT3Battery Contributors，同样以 MIT 授权。
