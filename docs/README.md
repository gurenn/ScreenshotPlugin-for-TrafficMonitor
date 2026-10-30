# TMPluginScreenshot — TrafficMonitor 截图插件

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
![Version](https://img.shields.io/badge/version-1.2.0-blue)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-lightgrey)

把常驻任务栏的 TrafficMonitor 变成"随手可截"的入口：任务栏窗口上的相机图标、宿主右键菜单里的
6 条插件命令、以及全局热键 `Ctrl+Alt+A`，三种方式任选其一即可完成 **全屏 / 区域 / 活动窗口**
截图，按预设格式与命名规则静默落盘，并给出成功或失败反馈。

- 宿主：TrafficMonitor 1.8.6（x64），插件接口 **API version 8**
- 技术栈：纯 C++17 + Win32 + WIC（`windowscodecs.lib`），无 MFC / ATL / GDI+ / 第三方库
- 运行库：`/MT` 静态 CRT，插件不依赖 `VCRUNTIME140.dll` / `MSVCP140.dll`
- 唯一导出：`extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance();`

---

## 1. 目录结构


```
TrafficMonitor-ScreenshotPlugin/
├─ build.bat                  主构建入口（验收以此为准）
├─ CMakeLists.txt             备选构建入口（与 build.bat 参数等价）
├─ include/PluginInterface.h  宿主官方插件接口头文件（只读，请勿修改）
├─ src/                       14 个源文件 / 头文件
│   ├─ PluginSettings.h/.cpp  平台头聚合、错误码、日志、ini 读写、i18n 文案表
│   ├─ CaptureEngine.h/.cpp   虚拟屏幕几何、全屏/区域/窗口抓取、守卫、光标
│   ├─ ImageWriter.h/.cpp     WIC 编码（PNG/JPG/BMP）、命名去重、剪贴板
│   ├─ RegionSelector.h/.cpp  区域选择覆盖层（分层窗口 + 模态消息循环）
│   ├─ OptionsDialog.h/.cpp   零资源模态设置对话框（内存 DLGTEMPLATE）
│   ├─ HotkeyManager.h/.cpp   全局热键（注册在宿主 UI 线程的 sink 窗口上）
│   └─ TMPlugin.h/.cpp        ITMPlugin / IPluginItem / 会话管线 / DllMain
├─ tools/
│   ├─ verify.bat             构建验收脚本（导出/依赖/位宽三项断言）
│   └─ smoketest.cpp          冒烟测试源码（可选，见 §10）
├─ docs/
│   ├─ README.md              本文件
│   ├─ ARCHITECTURE.md        系统设计
│   ├─ TASKS.md               任务分解
│   ├─ PRD.md                 需求
│   └─ manual-test.md         实机手工验收清单
├─ bin/TMPluginScreenshot.dll 构建产物
└─ build/                     中间文件（.obj / .lib / .pdb）
```

---

## 2. 环境要求

| 项 | 要求 |
| --- | --- |
| 操作系统 | Windows 10 1703 及以上（需要 `SetThreadDpiAwarenessContext`；更早系统会自动降级，功能不中断） |
| 编译器 | Visual Studio 2022 Build Tools（MSVC 14.3x）+ Windows SDK 10.x |
| 宿主 | TrafficMonitor 1.8.6 **x64**（插件必须与宿主同位数） |
| 磁盘/内存 | DLL 约 300 KB；运行时峰值取决于虚拟屏幕像素数 |

本机实测环境：`MSVC 14.44.35207` + `Windows SDK 10.0.26100.0`。

---

## 3. 构建

### 3.1 主构建（验收路径）

在 Git Bash / cmd 中执行：

```bash
cmd //c build.bat
```

产出 `bin/TMPluginScreenshot.dll`。全量重建：

```bash
cmd //c build.bat clean
```

`build.bat` 会：

1. `call vcvars64.bat` 设置 MSVC 环境；
2. 若 `vcvars64` 没能提供 Windows SDK 路径（部分加固环境下它依赖的注册表查询被禁用），
   自动扫描 `C:\Program Files (x86)\Windows Kits\10\Include` 并补齐 `INCLUDE` / `LIB`；
3. 用一条 `cl /LD` 命令编译 7 个 `.cpp` 并链接成 DLL；
4. 任一编译/链接错误返回非 0 退出码。

**注意**：脚本里不要引入 `MSBuild.exe`、`powershell.exe`、`WScript` 等；当前环境的安全策略会拦截。

### 3.2 备选构建（CMake）

```bash
cmake -S . -B build-cmake -A x64
cmake --build build-cmake --config Release
```

参数与 `build.bat` 严格等价（`/utf-8`、`/MT`、`/W3`、`/O2`、`_WIN32_WINNT=0x0A00`、
相同的链接库）。CMake 不在 PATH 时忽略本节即可。

### 3.3 构建验收

```bash
cmd //c tools\verify.bat
```

退出码 `0` 表示三项断言全绿：

| 断言 | 期望 |
| --- | --- |
| `dumpbin /exports` | 导出表里只有 1 个未修饰 C 符号 `TMPluginGetInstance` |
| `dumpbin /dependents` | 无 `VCRUNTIME*` / `MSVCP*` / `api-ms-win-crt*`；其余依赖全在白名单内 |
| `dumpbin /headers` | `machine (x64)` |

本机实测的依赖清单（7 项，全部为系统 DLL）：

```
dwmapi.dll  SHELL32.dll  ole32.dll  OLEAUT32.dll  USER32.dll  GDI32.dll  KERNEL32.dll
```

> `windowscodecs.dll` 不需要出现在依赖表里：WIC 通过 COM（`CoCreateInstance`）在运行时加载，
> 属于"用到才加载"，不是静态导入。静态 CRT 也只引入 `KERNEL32/USER32` 等系统 DLL。

---

## 4. 安装

1. 关闭 TrafficMonitor（托盘图标 → 右键 → 退出）。
2. 把 `bin\TMPluginScreenshot.dll` 复制到 `<TrafficMonitor 安装目录>\plugins\`。
   默认是 `C:\Program Files\TrafficMonitor\plugins\`（需要管理员权限）。
3. 启动 TrafficMonitor。
4. 右键任务栏窗口或主窗口 → **显示项目** → 勾选 **截图** —— 任务栏窗口与主窗口都会出现相机图标。
5. 右键菜单 → **截图插件** 子菜单，可见 6 条命令。

首次加载后会在宿主给出的插件配置目录生成：

- `TMPluginScreenshot.ini` —— 配置（UTF-16LE + BOM）
- `TMPluginScreenshot.log` —— 日志（UTF-8 + BOM，256 KB 滚动，只保留一份历史 `.1`）

## 5. 卸载

1. 退出 TrafficMonitor（托盘右键 → 退出）。
2. 删除 `plugins\TMPluginScreenshot.dll`。
3. 可选：删除插件配置目录下的 `TMPluginScreenshot.ini` / `.log`。

**线程模型与卸载语义**（这部分是本期缺陷修复的核心，改动过，请注意）：

| 线程 | 用途 | 生命周期 |
| --- | --- | --- |
| 宿主 UI 线程（`TID_H`） | `sink` 隐藏窗口、全局热键的 `Register/UnregisterHotKey`、接收 `WM_HOTKEY` / 抓屏结果并发通知 | 宿主进程 |
| 一次作业一个线程（`TID_Job`） | 延时等待 → 抓屏（含覆盖层模态循环）→ WIC 编码 → 落盘 → 剪贴板 | 随作业结束自行退出（句柄立即关闭，不 join） |

- **不再有常驻的热键线程和常驻抓屏线程。** 原因：DLL 自建线程在 `FreeLibrary` 时
  无法在 `DllMain(DLL_PROCESS_DETACH)` 内被 join —— 线程退出本身要拿 loader lock，
  而 loader lock 正被我们的 `DllMain` 持有。实测把两处等待放大到 3 s，`FreeLibrary`
  就精确地多花 6 s，说明 join 必然超时；超时后 `DllMain` 返回、映像被 unmap，线程再
  回来执行插件代码就是 `0xC0000005`。
- 因此 `DllMain(DETACH)` 只做三件事：`SetEvent` 取消信号、`UnregisterHotKey`、
  `DestroyWindow(sink)`，**不做任何等待**，宿主退出耗时是毫秒级。
- 热键注册在 sink 窗口所在的宿主 UI 线程上，注册/注销天然同线程；宿主退出后
  `Ctrl+Alt+A` 立刻可以被其它程序正常注册（实测 `FreeLibrary` 后 31–47 ms 完成）。
- **已知边界**：运行期被强制 `FreeLibrary` 时，如果恰好有一个作业正在跑（延时倒计时 /
  区域框选 / 编码落盘），那个作业线程可能还在执行插件代码。插件管理器类工具若要在运行期
  卸载本 DLL，请先停掉正在进行的截图。TrafficMonitor 1.8.6 本身从不在运行期卸载插件，
  正常使用不会遇到这种情况。

---

## 6. 使用

### 6.1 五条触发路径

| 路径 | 操作 | 结果 |
| --- | --- | --- |
| A 相机图标 | 左键单击任务栏窗口 / 主窗口上的相机图标 | 全屏截图（最常用） |
| B 全屏截图 | 右键 → 截图插件 → **全屏截图** | 抓取整个虚拟屏幕（所有显示器拼接） |
| C 区域截图 | 右键 → 截图插件 → **区域截图** | 出现遮罩，按住左键拖框，松开即确认；`Esc` / 右键取消；`Enter` 也可确认 |
| D 活动窗口截图 | 右键 → 截图插件 → **活动窗口截图** | 抓取当前前台窗口（自动剔除窗口阴影） |
| E 延时截图 | 右键 → 截图插件 → **延时截图** | 按设置里的"延时（秒）"等待后抓全屏 |
| F 全局热键 | 任意程序前台按 `Ctrl+Alt+A` | 等同路径 A（可在设置里改键或关闭） |

> 相机图标右键**不会**被插件吞掉，它由宿主弹出原生右键菜单（6 条命令就在里面）。
> 区域选择时选区小于 4×4 像素视为误触，直接取消且不提示。

### 6.2 设置对话框

右键 → 截图插件 → **设置…**

| 分组 | 控件 | 说明 |
| --- | --- | --- |
| 保存 | 保存目录 + 浏览… / 打开目录 | 显示展开后的绝对路径；目录不存在会自动创建 |
| 保存 | 图片格式 | PNG（默认）/ JPG / BMP；仅 JPG 时启用质量 |
| 保存 | JPG 质量 | 1–100，默认 90；用 `−` / `+` 微调，也可直接输入 |
| 保存 | 文件名模板 + 示例 | 实时预览；非法字符自动替换为 `_` |
| 保存 | 同时复制到剪贴板 | 默认勾选（`CF_DIB` + 注册格式 `PNG`） |
| 截图 | 延时（秒） | 0–30，默认 0；只影响"延时截图"命令 |
| 截图 | 包含鼠标光标 | 默认关闭 |
| 截图 | 启用全局热键 + 热键 | 默认 `Ctrl+Alt+A`；点热键按钮后按下新组合键录制 |
| 截图 | 立即截屏 | 用对话框内当前参数试截一次，结果显示在"就绪"位置，不关闭对话框、不弹宿主气泡 |
| 底部 | 确定 / 取消 / 恢复默认 | **只有"确定"写 ini**；"恢复默认"只重置控件 |

**热键**必须包含至少一个修饰键（Ctrl/Alt/Shift/Win）；只按修饰键会给出红色提示且不写 ini。
新热键被其它程序占用时，会在对话框内标红提示并**保留旧热键**，同时**不写 ini**。

### 6.3 文件命名

默认模板 `TMSnap_yyyyMMdd_HHmmss` → `TMSnap_20261029_153012.png`。

支持的占位符：`yyyy` `MM` `dd` `HH` `mm` `ss`。
非法字符 `\ / : * ? " < > |` 与控制字符替换为 `_`；结尾的 `.` 与空格被去掉；
`CON/PRN/AUX/NUL/COM1-9/LPT1-9` 等保留名前加 `_`；净化后超过 100 字符会截断。

重名不覆盖：同秒内第二次截图得到 `TMSnap_20261029_153012_1.png`（`_1`…`_999`，
都占满则追加 tick 随机后缀）。

---

## 7. 配置项说明

文件：`<插件配置目录>\TMPluginScreenshot.ini`，段名 `[TMSnap]`。
配置目录按以下优先级确定（三级兜底）：

1. `ITrafficMonitor::GetPluginConfigDir()`（宿主给出，通常是 `<TrafficMonitor>\plugins\config`）
2. `OnExtenedInfo(EI_CONFIG_DIR, dir)` 缓存值
3. `<DLL 所在目录>\config`；都不可写时退到 `%APPDATA%\TrafficMonitor\plugins\TMPluginScreenshot`，
   再失败则纯内存运行（能用，但设置不保存，并会提示）

| 键 | 类型 | 默认值 | 取值范围 / 回退 |
| --- | --- | --- | --- |
| `save_dir` | 字符串 | `%USERPROFILE%\Pictures\TMSnap` | 空/非法 → 默认值；支持环境变量 |
| `format` | 字符串 | `png` | `png` / `jpg`（也接受 `jpeg`）/ `bmp`；其它 → `png` |
| `jpg_quality` | 整数 | `90` | 1–100，越界钳制 |
| `file_name_template` | 字符串 | `TMSnap_yyyyMMdd_HHmmss` | 空 → 默认模板 |
| `copy_to_clipboard` | 0/1 | `1` | 非 0/1 → 默认 |
| `include_cursor` | 0/1 | `0` | — |
| `hotkey_enabled` | 0/1 | `1` | — |
| `hotkey_mods` | 字符串 | `ctrl+alt` | 形如 `ctrl+alt+shift+win`，解析失败 → 默认 |
| `hotkey_vk` | 整数 | `65`（A） | 0 或 >255 → 65 |
| `delay_seconds` | 整数 | `0` | 0–30，越界钳制 |
| `fallback_pictures_root` | 0/1 | `1` | 保存目录不可写时是否回退到"图片"根目录 |
| `last_saved` | 字符串 | 空 | 最近一次成功保存的完整路径（只记录，供"打开保存目录"定位） |

**逐字段回退**：`Load()` 对每个键单独读取，缺键/空值/非法值各自回退默认，绝不会因为一个键坏掉
而整体启动失败。手工删掉某个键后再启动插件照常工作。

> ini 首次创建时预写 UTF-16LE BOM（`FF FE`），否则 `WritePrivateProfileStringW` 会按 ANSI 写盘，
> 含中文的保存路径会乱码。若检测到已存在的 ini 不是 UTF-16，会重建为 UTF-16 并重写全部键。

---

## 8. 故障排查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 插件管理里看不到"截图插件" | DLL 位数不对 / 放错目录 / 被占用 | 确认是 x64 的 `TMPluginScreenshot.dll` 且位于 `plugins\` 子目录，重启宿主 |
| 提示"热键 Ctrl+Alt+A 已被占用" | 其它程序先注册了同一组合 | 打开设置改键（如 `Ctrl+Shift+1`），其余两条链路（图标/命令）不受影响 |
| 提示"目录不可写 — `<path>`" | 保存目录只读或不存在且无法创建 | 改到 `图片\TMSnap` 或 `D:\shots`；开启了 `fallback_pictures_root` 时会自动改存到"图片"根目录并提示"已改存到 …" |
| 提示"图像过大，已拒绝" | 边长 > 32767 或预估像素内存 > 256 MB | 降低分辨率/关闭超高分辨率拼接屏 |
| 提示"当前桌面不可截取" | 锁屏、UAC 安全桌面 | 解锁后在普通桌面截图 |
| 提示"未找到可截取的窗口" | 前台窗口被最小化，或前台就是插件自己的遮罩 | 先切换到目标窗口再执行命令 |
| 提示"图像编码组件不可用（WIC）" | 精简系统缺少 WIC | 图片仍会复制到剪贴板，可直接粘贴 |
| 杀软/SmartScreen 报警 | DLL 注入宿主 + 全局热键 + 抓屏，行为特征敏感；本期**不签名** | 把 DLL 加入白名单。插件不联网、不写注册表、不落地可执行文件，只写 ini、log 与图片 |
| 想确认插件做了什么 | 看日志 | `<插件配置目录>\TMPluginScreenshot.log`（UTF-8，含时间戳与错误码） |

---

## 9. 安全与行为边界

- **不联网**：没有任何 socket / HTTP 调用。
- **不写注册表**：只读系统信息（`GetSystemMetrics` / `GetFileAttributes` 等）。
- **不落地可执行文件**：只写 `.ini`、`.log` 和用户要求的图片。
- **不加壳、不打压缩**：源码级构建产物，便于审计。
- **不改宿主进程 DPI 感知**：只用 `SetThreadDpiAwarenessContext` 作用于插件自己的线程；
  设置对话框里的"立即截屏"也放在独立线程跑，宿主 UI 线程的 DPI 上下文从头到尾不变。
- **跨 DLL 边界只传** `const wchar_t*`、`void*`（`HICON`）与 POD 结构，不传 STL 容器/堆指针。

## 10. 冒烟测试（可选，非交付必需）

`tools/smoketest.cpp` 是一个独立控制台程序，它会 `LoadLibrary` 插件、逐条校验 `ITMPlugin` /
`IPluginItem` 契约，并用一个模拟的 `ITrafficMonitor` 驱动一次真实全屏抓屏与一次设置对话框开关。

```bash
# 与 build.bat 相同的环境变量
cl /nologo /EHsc /MT /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
   /Iinclude /Fo"build\\" tools\smoketest.cpp /Fe"bin\smoketest.exe" ^
   /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib
bin\smoketest.exe png      # 也可以传 jpg / bmp
```

输出写在 `out-test\`（图片、`dialog.bmp` 对话框截图、`config\` 配置与日志）。

---

## 11. 修订记录

### v1.0.1（2026-10-29，独立验收后的缺陷修复）

| 缺陷 | 现象 | 根因 | 修复 |
| --- | --- | --- | --- |
| DEF-01（阻断） | JPG 质量 95 与 10 生成的量化表（DQT）**字节级完全相同**，体积比 1.00 | WIC 的 JPEG 编码器在 `IWICBitmapFrameEncode::Initialize()` 时就把 property bag 读完并定下量化表，而 `ImageQuality` 是在 `Initialize` **之后**才写进 bag，被静默丢弃（`Write` 仍返回 S_OK，所以日志里也看不到失败） | 把 `PROPBAG2{dwType=PROPBAG2_TYPE_DATA, vt=VT_R4, pstrName=L"ImageQuality"}` + `VARIANT` 的写入整体移到 `Initialize` **之前**（`ImageWriter.cpp` EncodeToFile） |
| DEF-02（严重） | 做过一次截图后 `FreeLibrary` 必崩（20/20），`0xC0000005` 落在已卸载的插件映像内 | `DllMain(DLL_PROCESS_DETACH)` 持着 loader lock 去 join 自建线程；而线程退出本身要拿 loader lock，join 必然超时（实测等待放大到 3 s，`FreeLibrary` 精确多花 6 s）→ 超时后映像被 unmap，线程再回来执行插件代码就崩 | 去掉**两个常驻线程**：全局热键改为注册在宿主 UI 线程的 sink 窗口上；抓屏改为"一次作业一个线程"、随作业结束自行退出。`DllMain(DETACH)` 只发取消信号 + `UnregisterHotKey` + `DestroyWindow`，**不做任何等待**（`FreeLibrary` 从 6063 ms 降到 31–47 ms） |
| DEF-04（轻微） | 目录不可写时提示里路径重复：`目录不可写（<path> \| <path>）` | `EnsureSaveDir()` 的 `outErr` 本身就是路径，又和 `dir` 拼了一次 | 二者相同时只输出一份 |
| DEF-05（轻微，P2） | "立即截屏"在宿主 UI 线程上同步跑整条抓屏+编码（阻塞 0.3–0.7 s），并会给宿主 UI 线程设 DPI 上下文 | 走在 `WM_COMMAND` 里同步调用 | 改为投递到独立线程，结果经 `WM_SNAP_TEST_RESULT` 回投；宿主 UI 线程不阻塞、不改 DPI |
| 附带 | — | `IsDirectoryWritable` 用 `CREATE_NEW` 探测，残留探测文件会让它误判"不可写" | 改用 `CREATE_ALWAYS` + `FILE_FLAG_DELETE_ON_CLOSE` + 带进程号/时间戳的唯一文件名 |

### v1.0.2（2026-10-30，真机热键截图致宿主崩溃的修复）

| 缺陷 | 现象 | 根因 | 修复 |
| --- | --- | --- | --- |
| DEF-07（阻断，真机） | 在真实 TrafficMonitor 1.8.6 中，热键/命令截图**成功保存后约 0.5 s 宿主必崩**（`error.log`：崩溃帧为 exe+0xB9208 ← 插件+0x1408D ← 插件+0x150CE ← 插件+0x1633F ← `CallWindowProcW`） | **宿主接口版本不匹配**：插件用 master 分支的 `PluginInterface.h`（API 8）编译，其 `ITrafficMonitor` 有 12 个虚函数；而 1.8.6 宿主内置的接口是 **API 7，`ITrafficMonitor` 只有 10 个虚函数**，且 `CTrafficMonitorApp::GetAPIVersion()` 返回 0。插件调用 `GetMainWindowHwnd()`（槽 10）/`GetTaskbarWindowHwnd()`（槽 11）时读到宿主 vtable **末尾之后的垃圾内存**当函数指针，`call [rax+50h]` 跳进无效地址 → AV。方向规律：宿主调插件（`ITMPlugin`，插件实现为超集）永远安全；**插件调宿主（`ITrafficMonitor`）必须按宿主自报版本门禁**——这正是接口头注释"先判断接口版本"的含义 | ① `OnInitialize` 缓存 `m_appApiVersion = m_app->GetAPIVersion()`（槽 0，任何版本都安全）；② `InvalidateHostWindows()` 与 `ShowOptionsDialog` 的父窗口回退这两处，仅在 `m_appApiVersion >= 1` 时才调用槽 10/11，1.8.6 下跳过（仅损失即时重绘，无功能影响）；③ 顺带放开 `GetLanguageId`（槽 5）/`GetPluginConfigDir`（槽 6）的过严门禁——它们是接口最初版函数，所有宿主都有。`ShowNotifyMessage`（槽 4）从首版就存在，保留不门禁 |
| 验证 | 真实 1.8.6 宿主内直发 `WM_HOTKEY` ×3：3/3 截图成功保存（1920×1080），完整走通"捕获→保存→结果回传→`ShowNotifyMessage` 气泡"，宿主存活，`error.log` 无新增 | | |

### v1.0.3（2026-10-30，界面语言跟随修复）

| 缺陷 | 现象 | 根因 | 修复 |
| --- | --- | --- | --- |
| DEF-08（一般） | 中文系统下插件菜单/设置界面/通知全为英文 | 宿主 `config.ini` 的 `language = 0`（跟随系统）→ `ITrafficMonitor::GetLanguageId()` 返回 0 → 落入插件语言映射的 default（英文）。v1.0.2 放开 `GetLanguageId` 的过严门禁后首次真正读到该值 | `CUxText::Init`：宿主语言 ID 为 0 时改用 `GetUserDefaultUILanguage()` 跟随系统 UI 语言；顺带修正语言表（2057 是 en-GB 不算中文，补 5124 zh-MO）。`tools/lang_check.exe`：mock 宿主返回 0 时命令名必须为中文，PASS |
| 文案 | 设置对话框"关于"显示"宿主插件接口版本：8" | 显示的是插件自己的 `ITMPlugin::GetAPIVersion()`，并非宿主的 `ITrafficMonitor::GetAPIVersion()`（1.8.6 实际为 7），有误导 | 文案改为"插件接口版本：8（宿主 1.8.6 实际为 7）" |

**1.8.6 中区域截图入口**（源码核实）：右键点击任务栏/主窗口上的"截图"相机图标 → 弹出菜单中以插件名命名的子菜单（"截图插件"）→ 其下依次为：插件选项…/插件详细信息… + 分隔线 + **全屏截图 / 区域截图 / 活动窗口截图 / 延时截图 / 打开保存目录 / 设置…**。热键与"立即截屏"触发的都是默认截图（全屏）。

### v1.1.0（2026-10-30，双热键槽位 + 每槽位动作绑定）

| 变更 | 说明 |
| --- | --- |
| 双热键槽位 | 设置对话框"截图"区新增"热键 1 / 热键 2"两组【启用开关 + 动作下拉框 + 按键录制】，每个热键可独立绑定动作：全屏截图 / 区域截图 / 活动窗口截图 / 延时截图（延时用 delay_seconds）。热键 1 沿用旧 ini 键（hotkey_enabled/mods/vk，向后兼容，默认 Alt+Shift+Q→全屏），热键 2 默认关闭（Ctrl+Alt+R→区域截图） |
| ini 新键 | `hotkey1_action`、`hotkey2_enabled`、`hotkey2_mods`、`hotkey2_vk`、`hotkey2_action` |
| 冲突语义 | 两个槽位由 `CHotkeyManager::Apply` 原子换绑：任一启用槽注册失败则整体回滚到旧绑定并返回 false（对话框不写 ini）；两个启用槽按键相同时拒绝保存（kMsgHotkeyDuplicate） |
| 验证 | `tools/hotkey2_check.exe`：热键 2（Ctrl+Alt+R→全屏）直发 WM_HOTKEY id=2 触发 2/2 次截图落盘、禁用的槽位 1 对 id=1 无响应、FreeLibrary 干净。verify.bat 4 项 / qa_contract 101 断言 / smoketest / lang_check 全绿 |

> 注意：`tools/build_lang.bat` 同一轮只能编 lang_check / hotkey2_check 中的一个（sed 切换源文件），两个 exe 都已随 v1.1.0 构建并验证。

### v1.1.1（2026-10-30，"浏览"按钮卡死宿主对话框的修复）

| 缺陷 | 现象 | 根因 | 修复 |
| --- | --- | --- | --- |
| DEF-09（严重，真机） | 设置对话框点"浏览"后整个对话框"未响应" | `SHBrowseForFolderW`（新旧样式都）要求调用线程处于 COM STA 单元。TrafficMonitor 1.8.6 的 UI 线程从未调用 OleInitialize（宿主源码核实 0 处），且 COM 单元模式一旦被其它组件设定便随线程终身固定、不可逆——若 UI 线程为 MTA，在它上面同步调用浏览对话框必挂。实测复现：MTA 线程 + 旧代码 = 进程永久卡死 | **目录浏览挪到专用 STA 线程**（新线程 `OleInitialize` 必然成功，新旧样式按返回值自适应）：UI 线程只禁用按钮防连点，浏览结果经 `WM_SNAP_BROWSE_DONE` 消息移交（pidl 所有权随消息转移，对话框已销毁时浏览线程自清理）。宿主 UI 线程不再有任何阻塞可能。"打开保存目录"的 `SHOpenFolderAndSelectItems` 同类依赖，同样按需 OleInitialize 并配对释放 |
| 验证 | `tools/browse_check.exe`（可加 `--mta` 模拟宿主 MTA 线程）：点击浏览后对话框经 SendMessageTimeout 探测保持响应、浏览对话框弹出并可程序化关闭、对话框正常返回——**无 COM 与 MTA 两个场景均 PASS**；对照实验：旧代码在 MTA 场景进程永久卡死（90s 超时强杀），证明检测器有效。verify.bat / qa_contract / smoketest / lang_check / hotkey2_check 全绿 |

### v1.2.0（2026-10-30，窗口选择器 + 区域选择视觉升级）

| 变更 | 说明 |
| --- | --- |
| 活动窗口截图：窗口选择步骤 | 触发"活动窗口截图"（热键动作或右键命令）后先弹出**窗口选择列表**：列出所有可截取的顶层窗口（图标 + 标题 + 类名），仅含 可见/非最小化/有标题/非工具窗口/未被 cloak 的窗口，插件自身窗口被排除；**当前前台窗口置顶并默认选中**；双击 / 回车 / "确定" 截取，Esc / "取消" 静默退出（不弹气泡）。选中的窗口截取其 DWM 扩展边界（剔阴影），最小化的窗口在截取前会被再次校验 |
| 区域截图视觉升级 | ① 拖拽前显示**全屏十字线光标**（白色主线 + 深灰投影，中心 10px 挖空圆环 + 中心点，系统光标隐藏）；② 选择框增加**四角 L 形高亮角标**（3px 蓝色，20px 长）；③ 尺寸提示升级为**圆角胶囊**（深色底 + 细灰边）；④ 拖拽期间十字线自动隐藏。遮罩（选区外半透明压暗）与白色+深灰双描边保持不变 |
| 兼容性 | CaptureEngine 新增 `CaptureWindow(HWND)` 通用入口（原活动窗口路径重构为其特例）；`WindowPicker` 与区域覆盖层同在作业线程运行（同一既有约定）；热键/命令/显示项目三条触发路径与 ini 键均无变化 |
| 验证 | `tools/picker_check.exe`（端到端）：列表出现且目标置顶 → 程序化选中并确认 → 截图落盘且尺寸与目标窗口 DWM 扩展边界**逐像素一致**（386×293）→ 取消路径零产出；回归：verify.bat 4 项 / qa_contract 101 断言 / smoketest PNG·JPG·BMP / lang_check / hotkey2_check / browse_check（含 MTA 场景）全绿 |
