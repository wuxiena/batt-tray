# BattTray · 无线设备电量托盘

把无线鼠标、键盘、耳机、手柄的剩余电量直接画在任务栏上：**一台设备一个图标**，悬停就能看清品牌和百分比。纯 Win32 API，单文件 135 KB，无运行时依赖，空闲 CPU 为 0。

[![最新版本](https://img.shields.io/github/v/release/wuxiena/batt-tray?label=release&color=2ea44f)](https://github.com/wuxiena/batt-tray/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![平台](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078d4)

## 下载

> ### [⬇ 下载最新版 `BattTray.exe`](https://github.com/wuxiena/batt-tray/releases/latest)
>
> 免安装、无需 .NET / VC++ 运行库，下载后**双击即可运行**（Windows 10 / 11）。
> 仓库内的 [`dist/BattTray.exe`](dist/BattTray.exe) 与 Release 里的文件完全一致。

## 界面

| 托盘图标（浅色 / 深色任务栏） | 桌面浮窗 | 应用图标 |
| :---: | :---: | :---: |
| ![托盘图标](docs/tray-icon.png) | ![桌面浮窗](docs/preview.png) | ![应用图标](docs/app-icon-preview.png) |

| 显示 | 含义 |
| --- | --- |
| 亮色数字 | 正在实时接收 |
| 变暗的数字 | 设备休眠，显示上次读数（悬停提示写「N 分钟前」） |
| 绿 / 橙 / 红 | 电量 >50% / 21–50% / ≤20%，低电量弹一次气泡 |

## 使用

1. 下载运行；能读到电量的设备各占一个托盘图标。
2. **左键单击**图标 = 显示 / 隐藏桌面浮窗（可拖动，位置会记住）。
3. **右键菜单**：设备列表、桌面浮窗、开机自动启动、刷新间隔、使用统计、重新扫描设备、导出诊断信息、退出。

### 多设备

- 托盘图标按设备生成，插两个设备 = 两个图标；悬停提示最前面是品牌。
- **读不到电量的设备不占托盘**，只在右键菜单里列一行「电量不可读」，原因写在后面。
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

## 支持的设备

| 设备 | 电量读取方式 | 状态 |
| --- | --- | --- |
| 雷柏 Rapoo（VT 等鼠标） | 被动上报（只收不发，零功耗） | ✅ 实机验证 |
| 雷蛇 Razer | 90 字节特性报告查询 | ✅ 实机验证 |
| 罗技 Logitech | HID++ 2.0 | ⚠️ 按公开资料实现 |
| 华硕 ROG | 64 字节厂商查询 | ⚠️ 同上 |
| ATK / VXE / VGN / 达尔优 等 Compx 方案 | 短/长报告查询 | ⚠️ 同上 |
| LEOBOG / 迈从 MCHOSE（Realtek 方案） | 厂商命令查询 | ⚠️ 同上 |
| 飞智 Flydigi | FFA0 厂商查询 | ⚠️ 同上 |
| 小鸡 GameSir（Cyclone / G7 / Tarantula 等） | `3537` 厂商接口心跳 + 状态报文 | ⚠️ 按开源逆向实现 |
| 索尼 DualShock 4 / DualSense | 输入报文电量字段 | ⚠️ 按公开资料实现 |
| Xbox 及 XInput 兼容手柄 | 系统 API（4 档粗粒度） | ✅ 通用回退 |
| 蓝牙耳机（AirPods、索尼 WH/WF 等） | Windows 电池类接口 | ✅ 通用回退 |
| 西伯利亚 K03S、天剑PLUS 等 | dongle 不提供电量通道 | ❌ 确认读不到 |

- ⚠️ 项未在本机实机验证，按「尽力兼容」处理：解析失败不会显示可疑数字，只会回退到
  Windows 电池接口或标为「不可读」。
- 读不到电量的设备（最后一行）不占托盘图标，只在右键菜单注明；电量请看设备自身
  的指示灯或官方 App。

## 常见问题

**任务栏里看不到图标？**
Windows 11 默认把新图标收进「隐藏的图标」区（`^`）。点开 `^` 把 BattTray 拖出来，
或在 设置 → 个性化 → 任务栏 → 其它系统托盘图标 里打开常显。

**为什么我的耳机 / 手柄不显示电量？**
部分厂商的 2.4G dongle 根本不把电量交给电脑（如西伯利亚 K03S、天剑PLUS，均实测确认），
XInput 还常谎报「有线 / 满电」。这类设备程序不编数字：菜单里列一行「电量不可读」并注明原因。

**程序会不会很耗电？**
空闲 CPU 为 0；对设备只默认每 5 分钟主动查询一次（可调），被动上报机型零额外功耗。

## 编译

```bat
build.cmd
```

使用 [zig](https://ziglang.org/download/) 交叉编译（优先 `toolchain\ziglang\zig.exe`，否则 PATH 上的 `zig.exe`），
不需要 Visual Studio，产物为 `dist\BattTray.exe`。应用图标由 `tools/make_icon.py` 生成到
`src/app_icon.c`（字节数组，运行时 `CreateIconFromResourceEx` 构建）。

调试宏：`-DBATT_SIM`（模拟上报帧验证 UI）、`-DBATT_FAKE`（注入 4 台假设备验证多图标布局）。

## 数据存储

- 设置与缓存：`HKCU\Software\BattTray`（浮窗开关 / 位置、刷新间隔、读数缓存、使用统计 `H0..H11`）。
- 开机自启：`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，可在菜单里随时关闭。
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
