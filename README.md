# BattTray · 无线设备电量托盘

把无线鼠标、键盘、耳机、手柄的剩余电量直接画在任务栏上：**一台设备一个图标**，悬停就能看清品牌和百分比。纯 Win32 API，单文件约 120 KB，无运行时依赖，空闲 CPU 为 0。

[![最新版本](https://img.shields.io/github/v/release/wuxiena/batt-tray?label=release&color=2ea44f)](https://github.com/wuxiena/batt-tray/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078d4)

## 下载

> ### [⬇ 下载最新版 `BattTray.exe`](https://github.com/wuxiena/batt-tray/releases/latest)
>
> 免安装、无需 .NET / VC++ 运行库，下载后**双击即可运行**（Windows 10 / 11）。
> 仓库内的 [`dist/BattTray.exe`](dist/BattTray.exe) 与 Release 里的文件完全一致。

## 界面

| 托盘图标（浅色 / 深色任务栏） | 应用图标 |
| :---: | :---: |
| ![托盘图标](docs/tray-icon.png) | ![应用图标](docs/app-icon-preview.png) |

| 显示 | 含义 |
| --- | --- |
| 亮色数字 | 正在实时接收 |
| 变暗的数字 | 设备休眠，显示上次读数（悬停提示写「N 分钟前」） |
| 绿 / 橙 / 红 | 电量 >50% / 21–50% / ≤20%（颜色档位固定，提醒阈值可在菜单里改） |

## 使用

1. 下载运行；能读到电量的设备各占一个托盘图标。
2. **左键 / 右键**图标 = 打开菜单（设备列表、开机自启、刷新间隔、低电量提醒、使用统计等）。
3. **右键菜单**：设备列表、开机自动启动、刷新间隔、低电量提醒、使用统计、重新扫描设备、重置所有信息、导出诊断信息、卸载并清除所有数据、退出。

### 多设备

- 托盘图标按设备生成，插两个设备 = 两个图标；悬停提示最前面是品牌。
- **读不到电量的设备完全不显示**（不占托盘、也不进菜单）——电量请看设备自身的
  指示灯或官方 App。
- 所有设备都断开时会保留一个 `-` 图标当入口，否则菜单就再也打不开了。

### 断连与使用统计

- 设备关机或断连后，托盘图标和菜单条目会自动消失（被动上报型 3 分钟无帧、主动查询型连失 3 次）；
  设备重新开机会自己回来。
- 右键菜单 → **使用统计**：按「品牌 + 类别」累计连接时长，**只算连接中的时间**；
  支持一键清除历史记录。数据每 5 分钟落一次盘，重启不清零。

### 刷新间隔

右键菜单 → 刷新间隔：3 秒 / 1 分钟 / **5 分钟（默认）** / 10 分钟 / 30 分钟。

主动查询机型（雷蛇、ROG、小鸡等）每次查询都会叫醒一次无线设备的射频，间隔越短越耗电；
被动上报机型（雷柏、索尼手柄）不受此设置影响，托盘仍每 3 秒更新。

### 低电量提醒

右键菜单 → **低电量提醒**：开关、阈值（10% / 15% / **20%（默认）** / 30%）、「测试提醒气泡…」。

- 收到**实时读数**且电量 ≤ 阈值时，弹一条托盘气泡「品牌 类别 电量低 / 剩余 N%，建议充电。」
  气泡里显示的就是这一帧读数；**充电中**不提醒，开机后还没读到实时值的缓存读数也不提醒。
- 同一台设备每次连接只弹一次：只有电量回到「阈值 +5%」以上才复位，所以在阈值附近
  波动不会反复打扰。设备断连后重新上线、或程序重启，会重新具备提醒资格。
- **0% 防误报**：单帧 0% 常是空帧 / 无应答解出来的（v1.1.3 修过小鸡手柄这类情况），
  现在要连续两帧 0% 才允许提醒；托盘仍照常显示读数。超出 0..100 的读数直接丢弃。
- 气泡提交失败（图标还没注册、系统没接住）不会记为「已提醒」，下一次读数会自动重试。
- 看不到气泡时，先用「测试提醒气泡…」自检：Windows 11 的「隐藏的图标」区、
  专注助手 / 勿扰、系统通知开关都可能吞掉托盘气泡，程序无法绕过系统通知策略。

## 支持的设备

| 设备 | 电量读取方式 | 状态 |
| --- | --- | --- |
| 雷柏 Rapoo（VT 等鼠标） | 被动上报（只收不发，零功耗） | ✅ 实机验证 |
| 雷蛇 Razer | 90 字节特性报告查询 | ✅ 实机验证 |
| 罗技 Logitech | HID++ 2.0 | ✅ 实机验证 |
| 华硕 ROG | 64 字节厂商查询 | ⚠️ 按公开资料实现 |
| ATK / VXE / VGN / 达尔优 等 Compx 方案 | 短/长报告查询 | ⚠️ 同上 |
| LEOBOG / 迈从 MCHOSE（Realtek 方案） | 厂商命令查询 | ⚠️ 同上 |
| 飞智 Flydigi | FFA0 厂商查询 | ⚠️ 同上 |
| 小鸡 GameSir（Cyclone 2 / G7 Pro 8K PC / Tarantula Pro 8K） | `3537` 厂商接口 `0x12` 状态报文（第 36 字节电量、第 35 字节 bit0 充电） | ⚠️ 按开源逆向实现，未实机 |
| 索尼 DualShock 4 / DualSense | 输入报文电量字段 | ⚠️ 按公开资料实现 |
| Xbox 及 XInput 兼容手柄 | 系统 API（4 档粗粒度） | ✅ 通用回退 |
| 蓝牙耳机（AirPods、索尼 WH/WF 等） | Windows 电池类接口 | ✅ 通用回退 |

- ⚠️ 项未在本机实机验证，按「尽力兼容」处理：解析失败不会显示可疑数字，只会回退到
  Windows 电池接口或标为「不可读」。
- 读不到电量的设备完全不显示，电量请看设备自身的指示灯或官方 App。
- 小鸡 GameSir 只有部分型号在 Windows 上能读到电量：Cyclone 2、G7 Pro 8K PC、
  Tarantula Pro 8K 走 HID 厂商接口；G7 Pro（非 8K）有线 / 接收器走厂商类 USB、
  G7 SE / Kaleid 走 GIP、老款 T4 Pro / G4 Pro 只有 LED 档位指示，这几类读不到，
  所以不显示。手柄需切到 **Xbox 模式**才会推电量报文。

## 常见问题

**任务栏里看不到图标？**
Windows 11 默认把新图标收进「隐藏的图标」区（`^`）。点开 `^` 把 BattTray 拖出来，
或在 设置 → 个性化 → 任务栏 → 其它系统托盘图标 里打开常显。

**为什么我的耳机 / 手柄不显示电量？**
部分厂商的 2.4G dongle 根本不把电量交给电脑（已实测确认），
XInput 还常谎报「有线 / 满电」。这类设备程序无法显示电量，所以直接不显示；
电量请看设备自身的指示灯或官方 App。

**低电量提醒没弹，或者气泡里的数字不对？**
先用「低电量提醒 → 测试提醒气泡…」确认 Windows 通知是否正常，再导出 `BattTray-diag.txt`
看该设备的 `protocol` 与 `level`。v1.1.3 及更早版本气泡里显示的是**上一帧**读数
（首次读数会显示注册表缓存值，设备离线复活时甚至是 -1%），v1.1.4 起改为显示当前读数。
另外充电中不会提醒，单帧 0% 也不会立刻提醒。

**程序会不会很耗电？**
空闲 CPU 为 0；对设备只默认每 5 分钟主动查询一次（可调），被动上报机型零额外功耗。

## 编译

```bat
build.cmd
```

使用 [zig](https://ziglang.org/download/) 交叉编译（优先 `toolchain\ziglang\zig.exe`，否则 PATH 上的 `zig.exe`），
不需要 Visual Studio，产物为 `dist\BattTray.exe`。

应用图标：`tools/make_icon.py` 生成 `src\res\app.ico`，构建时由 `tools\embedres.c`
经 Win32 `UpdateResource` 写进 exe 的资源段（RT_GROUP_ICON + RT_ICON），
所以资源管理器 / 任务栏 / Alt+Tab 显示的都是电池图标。

调试宏：`-DBATT_SIM`（模拟上报帧验证 UI）、`-DBATT_FAKE`（注入 4 台假设备验证多图标布局）。

## 数据存储

- 设置与缓存：`HKCU\Software\BattTray`（刷新间隔 `IV`、低电量提醒开关 `LO` 与阈值 `LT`、
  读数缓存 `N/L/S`、使用统计 `H0..H11`）。「重置所有信息」只清缓存与统计，保留这些设置。
- 开机自启：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，可在菜单里随时关闭。
- **卸载**：右键菜单 →「卸载并清除所有数据」，会清空上面的注册表数据、移除开机自启，
  并把程序文件标记为下次重启时自动删除（程序运行中删不掉自己，所以延迟到重启）。
- **导出诊断信息**：右键菜单可生成 `BattTray-diag.txt`（本机全部 HID 集合、协议匹配结果、使用统计），
  遇到识别问题欢迎附上它开 issue。

## 关于本项目

**这个项目的代码、README 和截图脚本都是 AI 写的。** 作者提出需求、在真实硬件上测试并验收；
代码与文档由 AI 编程助手在对话中生成。

## 许可证与参考

本项目基于 [MIT](LICENSE) 协议开源。协议实现参考了以下项目的公开资料（协议常量与帧格式，
未复制任何代码）：

- [JD13703202/rapoo-vt3-battery](https://github.com/JD13703202/rapoo-vt3-battery)（MIT）— 雷柏协议
- [XAVIER0229/GearPower](https://github.com/XAVIER0229/GearPower)（MIT）— 雷蛇 / 罗技 / ROG / Compx 等实机报文
- [rafagfran/mchose-v9-pro-battery-tray](https://github.com/rafagfran/mchose-v9-pro-battery-tray)（MIT）— 0xFF90 耳机通道
- [broroeror/gamesir-linux-tools](https://github.com/broroeror/gamesir-linux-tools) — 小鸡厂商接口
- [openrazer/openrazer](https://github.com/openrazer/openrazer)（GPL-2.0）、
  [ForceLightning/libatk-rs](https://github.com/ForceLightning/libatk-rs)（GPL-3.0）— 雷蛇 / ATK 命令结构
