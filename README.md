# scrdock — scrcpy 悬浮工具栏

纯 C / Win32 实现的 scrcpy 悬浮控制条。把 `scrdock.exe` 放到 scrcpy 所在目录
（或任意目录，只要能找到 `scrcpy.exe`），运行后会：

1. 自动拉起 `scrcpy.exe`（若已有实例在运行则直接附着，不重复启动）
2. 在 scrcpy 镜像窗口**右侧边缘贴合**停靠一个竖条工具栏（不进任务栏、
   不抢键盘焦点、层级挂在 scrcpy 窗口上），并实时跟随 scrcpy 窗口的
   移动/缩放/最小化/全屏
3. 控制注入走两条通道：
   - **快捷键通道（默认，即时）**：向 scrcpy 窗口投递 `WM_SYSKEYDOWN`
     （MOD=lalt + H/B/S/P/↑/↓/N），scrcpy 经自己的控制 socket 注入，
     延迟约一帧；拉起时自动加 `--shortcut-mod=lalt` 固定 MOD
   - **adb 通道（回退/附着模式）**：`adb shell input keyevent`
     （`input` 每次冷启 Java，USB 下有 300ms+ 延迟，故仅作回退）

| 按钮 | 功能 | 快捷键 | keycode |
|---|---|---|---|
| ⌂ | 主屏幕 Home | Alt+H | 3 |
| ⃪ | 返回 Back | Alt+B | 4 |
| ▭ | 最近任务 | Alt+S | 187 |
| ⏻ | 电源（灭屏，镜像继续） | Alt+P | 26 |
| 🔊/🔇 | 音量 +/− | Alt+↑/↓ | 24 / 25 |
| 🔔 | 展开通知栏 | Alt+N | cmd statusbar（回退 swipe） |
| ⚙ | 展开控制中心 | adb 直开（单开，不连带通知栏） | cmd statusbar expand-settings |
| 📌 | 窗口置顶开关（scrcpy + 工具栏一起置顶/取消） | HWND_TOPMOST | — |
| 📷 | 截图 → `%USERPROFILE%\Pictures\scrdock\` | adb exec-out | — |
| ✕ | 关闭工具栏（默认同时退出 scrcpy） | — | — |

## 交互

- **悬停**：按钮高亮 + 350ms 后显示功能提示（自绘 tooltip 气泡，跟随光标）
- **拖动**：按住按钮以外的背景拖动 → 进入手动模式（不再跟随）
- **双击背景** 或 **右键 → 重新吸附**：恢复跟随
- **右键**：菜单（重新吸附 / 退出）
- **置顶开关**：📌 同时给 scrcpy 窗口和工具栏设置/取消 HWND_TOPMOST（激活时按钮蓝色高亮，窗口重建后自动恢复）
- **层级**：工具栏是 scrcpy 窗口的 owned 弹窗（QtScrcpy 的 Qt::Tool 同款思路）——
  永远在 scrcpy 之上（包括全屏），但 scrcpy 被其他窗口盖住时工具栏一起下沉，
  不会悬浮在无关窗口上
- 点击按钮不会抢走 scrcpy 窗口的键盘焦点（`WS_EX_NOACTIVATE`）

## 跟随原理（SetWinEventHook 线程约束）

钩子以 `WINEVENT_OUTOFCONTEXT` 安装在跑消息循环的 UI 线程上，事件由系统
编组进该线程的队列、在泵消息时回调——**回调与消息循环同线程，绝不跨线程**。
主循环用 `MsgWaitForMultipleObjectsEx(MWMO_INPUTAVAILABLE)` 同时等
scrcpy 进程句柄和消息：scrcpy 退出 → 工具栏自动退出，全程消息不断流。

事件集：`EVENT_OBJECT_LOCATIONCHANGE`（移动+缩放都会触发）、
`EVENT_OBJECT_SHOW`（SDL 窗口首帧显示）、`EVENT_OBJECT_DESTROY`、
`EVENT_SYSTEM_MINIMIZESTART/END`；回调内先过滤
`idObject==OBJID_WINDOW && idChild==CHILDID_SELF`（否则鼠标光标移动会每分钟
触发上千次回调）。另有 500ms `TIMER_SYNC` 兜底自愈。层级用 `GWLP_HWNDPARENT`
挂到 scrcpy 窗口（owned 弹窗），不依赖置顶标志。

## 配置（scrdock.ini，与 exe 同目录，可选）

```ini
[scrdock]
; 多设备时指定序列号（单设备可省略）
serial=6e52b59e
; 附加传给 scrcpy 的参数（原样追加，如 --max-size=800）
scrcpy_args=--stay-awake
; 自定义 adb.exe 路径（默认: 同目录 → scrcpy 目录 → PATH）
adb=D:\platform-tools\adb.exe
; 关闭工具栏时是否同时退出 scrcpy（附着模式下永不退出）默认 1
close_scrcpy_on_exit=1
; 控制通道: auto(默认: 自启动用快捷键, 附着用 adb) | shortcut | adb
; 注意:附着的外部实例若改过 --shortcut-mod,请用 adb
control=auto
```

注意：ini 按 ANSI 解析，值请用 ASCII（序列号/路径本来就是 ASCII）。

## 编译

需要 MSVC（VS2019+ / BuildTools）。仓库 VS18 已验证：

```bat
cd /d D:\Test\scrcpy\scrdock
build.bat                          :: 产物 build\scrdock.exe
build.bat D:\path\to\scrcpy\dist   :: 编译并复制到 scrcpy 目录
```

零外部依赖：只链 user32/gdi32/kernel32/shell32/advapi32。

## 图标与资源

`scrdock.ico`（16~256 共 7 个尺寸）经 `scrdock.rc` 内嵌进 exe（含 VERSIONINFO 1.0.0.0），
窗口类也挂了该图标。ico 由 `mkicon.ps1` 矢量绘制生成（设计 = 产品自喻：
左边一块投屏 + 右边贴一条工具栏，中间按钮绿色强调），改配色/布局后重跑即可再生成。

## 已知限制

- 附着模式下默认走 adb 通道（外部实例的 --shortcut-mod 未知，投 Alt+X
  会被当作普通按键转发到设备）；确知 MOD=lalt 时可设 `control=shortcut`
- 电源键（26）是真实电源键：会灭屏（镜像继续）
- ini 值为 ANSI；截图目录固定在 `Pictures\scrdock`
- 同一时间只允许一个 scrdock 实例（互斥体）
- 检测到多个 scrcpy.exe 时附着最新的那个；多个 Android 设备时建议在
  ini 里写死 serial

