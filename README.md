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
| ⋯ | **管理窗口**：设备 / 参数 / 路径 三页 | — | — |
| ✕ | 关闭工具栏（默认同时退出 scrcpy） | — | — |

## 管理窗口（⋯ 按钮）

- **设备页**：1.5 秒轮询 `adb devices`（仅变化时刷新，插拔/授权约 1~2 秒内反映），
  列出状态（在线/离线/待授权）、序列号、型号、Android 版本、连接类型、电量、
  **投屏状态**（●当前 = 工具栏正在控制的一路，● = 后台会话）；
  选中一行 → **连接所选**（双击同效）：已有会话则切换为活动会话（不重启），
  否则新开一路；**断开**关闭选中行（或当前）的会话；勾选
  **记住此设备** 则把 `serial=` 写进 ini（不勾 = 自动模式：跟随唯一在线设备）。
- **无线行**：`无线(--tcpip)` 开关（经 USB 一键切 WiFi）、无线地址手动
  `adb connect`、**配对…** 打开 Android 11+ 无线配对窗（默认支持**生成二维码手机扫码自动配对**与连接，亦可切换为手动输入 ip:端口 + 配对码）。
- **参数页**：码率 / 最大尺寸 / 最大帧率 / 息屏镜像 / 保持唤醒 / 禁用音频 /
  显示触摸点 做成控件；`scrcpy_args` 保留为"附加参数"追加在末尾（可覆盖前者）；
  断线自动重连及其次数；关闭工具栏时是否退出 scrcpy。
- **路径页**：scrcpy.exe / adb.exe 路径（留空 = 自动查找：同目录 → scrcpy 目录
  → PATH），检测版本按钮，保存后立即生效（更换 adb 会重启设备监测）。

## 多设备多开（最多 4 路）

每台设备各起一路 scrcpy（各自 `-s`），同时投屏；**工具栏只有一个**，控制
"活动会话"——在设备页选中某行点 连接所选 即切换目标（工具栏重新吸附到该
窗口，快捷键/adb 控制随切随到）。活动会话断线自动重连；后台会话结束只在
列表徽标消失。工具栏退出时全部会话一并关闭。

## 无线连接

- **扫码配对（Android 11+）**：点击管理窗口设备页的【配对…】，默认显示包含标准无线调试协议（`WIFI:T:ADB;S:scrdock-xxxx;P:xxxxxx;;`）的二维码。手机在“无线调试”中选择“使用二维码配对设备”扫描后，程序自动侦测并捕获手机 mDNS 广播（`_adb-tls-pairing`），自动完成 `adb pair` 与后续连接，全程无需手动输入任何字符。同时保留“手动配对”页签以防 AP 隔离等网络限制
- `无线(--tcpip)` 勾选后，下次连接经 USB 自动切换到 WiFi（scrcpy `--tcpip`）；
  设备序列号随之变为 `ip:5555`，断线重连会自动跟随新序列号
- USB 设备的 WiFi 地址会自动记住（ini `[devices]` 节），WiFi 掉线后重连
  前自动补发 `adb connect ip:5555`
- Android 11+ 设备配对成功后自动通过 `adb mdns services` 发现连接端口；
  无线地址留空点击“连接”也可重新发现。单一端点自动尝试连接，多个端点时
  回填第一项，请核对手机的连接地址后确认。发现服务不代表已配对，连接仍由 adb 验证。
- 未发现设备时最多查询 5 次，间隔 2 秒（每次查询另有 8 秒超时）；
  查询失败、网络不支持 mDNS 或自动连接失败时，可手动输入无线调试页的连接地址。

**断线自动重连**（默认开，5 次）：scrcpy 以退出码 2 结束（设备断开）时，等设备
重新出现在 `adb devices` 后自动带 `-s` 重拉；会话稳定超过 10 秒则重置次数。
启动失败（退出码 1）不重连，工具栏保持存活并在管理窗口显示错误。

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
scrcpy 进程句柄和消息：scrcpy 退出 → 投递 `WM_APP_PROCEXIT`（带退出码
与进程代号），正常关闭即退出工具栏，异常退出则显示错误/触发重连，全程消息不断流。

事件集：`EVENT_OBJECT_LOCATIONCHANGE`（移动+缩放都会触发）、
`EVENT_OBJECT_SHOW`（SDL 窗口首帧显示）、`EVENT_OBJECT_DESTROY`、
`EVENT_SYSTEM_MINIMIZESTART/END`；回调内先过滤
`idObject==OBJID_WINDOW && idChild==CHILDID_SELF`（否则鼠标光标移动会每分钟
触发上千次回调）。另有 500ms `TIMER_SYNC` 兜底自愈。层级用 `GWLP_HWNDPARENT`
挂到 scrcpy 窗口（owned 弹窗），不依赖置顶标志。

## 配置（scrdock.ini，与 exe 同目录，可选）

```ini
[scrdock]
; 多设备时指定序列号（空 = 自动模式：跟随唯一在线设备；管理窗口可写）
serial=6e52b59e
; 显式 scrcpy.exe 路径（默认: 同目录 → PATH 自动查找）
scrcpy=D:\Tools\scrcpy\scrcpy.exe
; 下次连接经 USB 切到 WiFi（scrcpy --tcpip）
wireless=0
; 附加传给 scrcpy 的参数（原样追加在末尾，可覆盖结构化选项）
scrcpy_args=--stay-awake
; 自定义 adb.exe 路径（默认: 同目录 → scrcpy 目录 → PATH）
adb=D:\platform-tools\adb.exe
; 关闭工具栏时是否同时退出 scrcpy（附着模式下永不退出）默认 1
close_scrcpy_on_exit=1
; 控制通道: auto(默认: 自启动用快捷键, 附着用 adb) | shortcut | adb
; 注意:附着的外部实例若改过 --shortcut-mod,请用 adb
control=auto
; —— 结构化参数（管理窗口"参数页"生成，0/缺省 = 不传给 scrcpy）——
bitrate=8          ; 视频码率 Mbps（--video-bit-rate=8M）
max_size=0         ; --max-size
max_fps=0          ; --max-fps
turn_screen_off=0  ; --turn-screen-off（息屏镜像）
stay_awake=0       ; --stay-awake
no_audio=0         ; --no-audio
show_touches=0     ; --show-touches
; 断线自动重连（默认 1 = 开）
auto_reconnect=1
reconnect_attempts=5
```

以上键均可由管理窗口读写；手改 ini 与管理窗口互不冲突（保存时按"空值删键"
原则只落非默认项）。

注意：ini 按 ANSI 解析，值请用 ASCII（序列号/路径本来就是 ASCII）。

## 编译

需要 MSVC（VS2019+ / BuildTools）。仓库 VS18 已验证：

```bat
cd /d D:\Test\scrcpy\scrdock
build.bat                          :: 产物 build\scrdock.exe
build.bat D:\path\to\scrcpy\dist   :: 编译并复制到 scrcpy 目录
```

零外部依赖：只链 user32/gdi32/kernel32/shell32/advapi32/comctl32/comdlg32。

## 图标与资源

`scrdock.ico`（16~256 共 7 个尺寸）经 `scrdock.rc` 内嵌进 exe（含 VERSIONINFO 1.0.0.0），
窗口类也挂了该图标。ico 由 `mkicon.ps1` 矢量绘制生成（设计 = 产品自喻：
左边一块投屏 + 右边贴一条工具栏，中间按钮绿色强调），改配色/布局后重跑即可再生成。

## 已知限制

- 附着模式下默认走 adb 通道（外部实例的 --shortcut-mod 未知，投 Alt+X
  会被当作普通按键转发到设备）；确知 MOD=lalt 时可设 `control=shortcut`
- 电源键（26）是真实电源键：会灭屏（镜像继续）
- 单实例多会话：最多 4 路投屏同时运行，一个工具栏控制活动会话
- ini 值为 ANSI；截图目录固定在 `Pictures\scrdock`
- 同一时间只允许一个 scrdock 实例（互斥体）
- 检测到多个 scrcpy.exe 时附着最新的那个；多设备时用管理窗口选择
  （拉起的 scrcpy 总是带 `-s <序列号>`，绝不连错设备）
- 后台（非活动）会话断线不自动重连；切换为活动会话后恢复重连

