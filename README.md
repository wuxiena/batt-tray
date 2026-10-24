# RapooBattery

一个 **34 KB** 的原生 Windows 托盘小程序：显示雷柏（Rapoo）2.4G 无线鼠标的电量。

Rapoo 的 2.4G 接收器不会被 Windows 登记为电池设备，系统设置里看不到电量。本项目通过逆向
厂商私有 HID 协议直接读取，并以纯 Win32 + 被动监听的方式实现，常驻内存仅约 3 MB、空闲 CPU 为 0。

![桌面浮窗](docs/preview.png)

## 特性

| 项目 | 数值 |
| --- | --- |
| 可执行文件 | 34 KB（静态链接，无运行时依赖，不需要 .NET / Python / VC++ 运行库） |
| 私有内存 | 约 3.3 MB |
| 工作集 | 约 14.5 MB（其余为 user32 / gdi32 等系统共享 DLL） |
| 空闲 CPU | 0（不轮询、不定时器） |
| 线程数 | 1 |

单线程 + overlapped `ReadFile` 常驻挂起，用 `MsgWaitForMultipleObjects` 等设备上报，
没有轮询循环，所以放着不动时几乎不耗电。

## 使用

1. 直接运行 `dist/RapooBattery.exe`（源码仓库内置了预编译版本；自行编译见下文）。
2. 托盘出现电池图标，悬停显示 `雷柏无线鼠标  电量 93%`。
3. 桌面右下角有一个黑底大号百分比的**浮窗**，可拖动，位置会记住。
4. **左键单击**托盘图标 = 显示 / 隐藏浮窗。
5. **右键菜单**：桌面浮窗、开机自动启动、重新连接、原始报文、退出。

### 显示状态

| 显示 | 含义 |
| --- | --- |
| 亮色数字 + 亮色图标 | 正在实时接收（鼠标醒着，约每 3 秒一帧） |
| 变暗的数字 / 图标 | 鼠标休眠或刚开机，显示的是上次读数，悬停提示会写「N 分钟前」 |
| `--` | 尚未读到过数据，动一下鼠标即可 |

颜色：绿 = >50%，橙 = 21–50%，红 = ≤20%（低电量时弹一次气泡提醒）。

## 原理（逆向结果）

测试设备为 `USB\VID_24AE&PID_1849`，设备名 `Rapoo Gaming Device`，是一个 2.4G 复合设备，
同时暴露键盘、系统控制、用户控制和一个厂商自定义集合。

- 关键接口：HID 厂商集合 `UsagePage 0xFF00 / Usage 0x0002`，输入报告长度 **7 字节**
  （消费电子视角即设备路径里的 `col05`），读写通道都可用。
- 鼠标醒着时**每 3 秒主动上报一帧**：

  ```
  BB B0 02 72 06 01 5D
  ↑              ↑
  报告类型 0xBB   电量 = 0x5D = 93%
  ```

- 末字节即电量百分比（0–100）。用 XOR / 累加校验推演都不成立，可排除它是校验位。
- 写通道能写成功，但设备不回包（休眠时更不响应查询），因此只能**被动监听**——
  这也正是本程序不做轮询的原因。
- Windows 不会把这类 2.4G 接收器登记成「电池设备」，所以系统设置里没有电量项。

## 编译

```bat
build.cmd
```

不需要 Visual Studio，使用 [zig](https://ziglang.org/download/) 的 `zig cc -target x86_64-windows-gnu`
交叉编译，产物为 `dist\RapooBattery.exe`。

`build.cmd` 会优先使用仓库内 `toolchain\ziglang\zig.exe`，找不到则回退到 PATH 上的 `zig.exe`。
工具链体积较大，未纳入版本库（已在 `.gitignore` 中排除），需要时自行解压到该路径。

调试构建：

| 宏 | 作用 |
| --- | --- |
| `-DRB_DEBUG` | 把调试日志写到 `%TEMP%\raboo_dbg.log` |
| `-DRB_SIM` | 每 1.5 秒注入一帧真实报文，用于离线验证 UI |

## 兼容性

仅对 VID `0x24AE` 且带「7 字节、`FF00/0002` 输入集合」的接收器生效（雷柏 VT 系列等游戏鼠标）。
若你的型号协议不同，用 `tools/probe/` 下的脚本重新抓一帧即可：解析逻辑集中在 `src/RapooBattery.c`
的 `handle_report()`，改一行就能适配。

## 目录结构

```
dist/RapooBattery.exe    预编译成品
src/RapooBattery.c       全部源码（单文件）
build.cmd                一键编译
docs/preview.png         README 截图
tools/                   调试与逆向辅助
  probe/probe_hid.py       枚举雷柏 HID 集合、capabilities、报告描述符
  probe/probe_overlapped.py 重叠读取原始上报帧
  capture.py               屏幕截图（纯 ctypes）
  findwin.py               枚举本程序窗口位置
  make_icon.py             生成 .ico
  icontest.c               图标渲染测试
```

## 设置存储

`HKCU\Software\RapooBattery`：`Float`（浮窗开关）、`FloatX` / `FloatY`（位置）、`Level` / `Stamp`（上次读数缓存）。
开机自启写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，可随时在菜单里关闭。

## 已知限制

- 充电状态读不出来（协议中未解析出可靠字段），因此不显示「充电中」。
- 鼠标休眠时不产生上报，只能显示上次读数（会明确变暗提示）。
- exe 未内嵌图标资源（zig 0.14 的 `zig rc` 在构建机上不可用）；托盘与浮窗图标为运行时 GDI 绘制，
  观感不受影响，仅资源管理器里显示为默认图标。

## License

[MIT](LICENSE)
