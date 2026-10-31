# 在 Vita3K 中运行

> Vita3K 结果只代表模拟器，不代替 PS Vita 真机。模拟器版本、后端和图形驱动都会影响
> 行为；每次报告必须记录版本与后端。

## 1. 准备 Vita3K

1. 安装 Vita3K（当前项目以 **OpenGL** 后端为基线）。
2. 使用 Vita3K 的固件安装功能安装你自己合法持有的 PS Vita 固件。
3. 确认能启动任意自制 VPK，排除固件/驱动问题。

项目不附带 Vita3K 或固件。

### 1.1 版本兼容性

- 必须使用 OpenGL 后端；Vulkan 不是当前验收路径。
- 某些新版 Vita3K 在加载器第一次 VM 分配时就拒绝（要求 1 MiB 倍数/16 MiB 上限）。
  这属于 emulator/loader 布局限制，不是图形问题。
- 如果启动即失败，先换一个已知能进入游戏的 Vita3K 构建，并在报告中记录可执行文件
  SHA256。不要通过修改游戏输入或跳过身份检查来适配。

## 2. 配置 Vita3K 路径

方式一：环境变量（推荐）

```powershell
$env:MHGU_VITA3K = "C:\Vita3K\Vita3K.exe"
```

方式二：本地选择文件（Git 忽略）

```json
// config/vita3k-runtime.json
{
  "emulator": "../Vita3K/Vita3K.exe"
}
```

方式三：私有运行时目录

```powershell
# 从 ../vita3k 硬链接 .exe/.dll，并 junction UI 资源，不修改原安装
.\tools\runtime\prepare_vita3k_runtime.ps1 -Destination "build/vita3k-runtime"
```

注意：项目当前实际使用 `build/vita3k-pref` 作为模拟器用户目录（pref），其中
`ux0/data/mhgu-vita-port` 是运行时数据。

## 3. 准备运行时数据

```powershell
.\scripts\prepare-runtime.ps1 `
  -RomFs "D:\MHGU_vita_port\game-input\romfs" `
  -ShaderPackage "D:\MHGU_vita_port\local\shader-package"
```

确认：

```text
build/vita3k-pref/ux0/data/mhgu-vita-port/
├─ romfs.index
├─ romfs.map
├─ romfs-tree/
├─ offline-ngword2.romfs
├─ render-resolution.txt
├─ static-shaders/
└─ savedata/          （可为空）
```

## 4. 启动

推荐使用带存档保护的包装：

```powershell
# 正常游玩；启动前自动备份
.\scripts\play.ps1

# 有界自动化运行：330 秒退出，230/310 秒截图，要求正常退出
.\scripts\play.ps1 `
  -ExitAfterSeconds 330 `
  -CaptureSeconds 230,310 `
  -RequireGracefulExit

# 退出后恢复启动前存档
.\scripts\play.ps1 -RestoreAfter
```

`play.ps1` 会把额外参数传给 `tools/runtime/run_gui_play.ps1`。

直接调用：

```powershell
.\tools\runtime\run_gui_play.ps1 `
  -Vpk "build\vita\vita_port_native_loader.vpk" `
  -Emulator "C:\Vita3K\Vita3K.exe" `
  -ExitAfterSeconds 330 `
  -CaptureSeconds 230,310 `
  -RequireGracefulExit
```

### 4.1 runner 做什么

1. 检查没有正在运行的 Vita3K 实例。
2. 校验 `config/render-resolution.txt`（只接受 360p/540p）。
3. 选择静态着色器源包，验证 manifest、index、sidecar、GXP 哈希。
4. 把 VPK 解压安装到 `build/vita3k-pref/ux0/app/MHGUVP018`。
5. 写入 play/display/media marker，隔离冲突诊断 marker。
6. 生成 `offline-ngword2.romfs`，复制分辨率配置。
7. 启动 Vita3K，验证 OpenGL 后端初始化。
8. 按计划截图，检查窗口可见性/尺寸/前台状态。
9. 按 `-ExitAfterSeconds` 停止或等待自然退出。
10. 在 `build/runs/play-<timestamp>/` 写 `process.json`、日志、截图和回执。

### 4.2 常用参数

| 参数 | 说明 |
|---|---|
| `-ExitAfterSeconds N` | N 秒后关闭；0 表示不自动关闭 |
| `-CaptureSeconds 60,300` | 在这些秒数截图 |
| `-NoWindowCapture` | 不做窗口截图（仅性能/诊断） |
| `-KeepRunning` | 保持运行，不自动关闭 |
| `-RequireGracefulExit` | 要求自然退出，否则失败 |
| `-PlayPerf` | 启用周期性能计时 |
| `-BoundedBatch` | 显式启用有界 Scene 批处理（当前默认策略见下） |
| `-DiagnosticMode` | 保留诊断 marker，不自动写入 play 路径 |
| `-Replay <file>` | 输入回放文件 |
| `-LiveInput` | 实时输入邮箱 |
| `-StaticShaderSource <dir>` | 显式指定静态着色器源包 |
| `-SilentAudio` | 使用静音音频诊断 |

### 4.3 当前默认渲染策略

- 分辨率：540p（`config/render-resolution.txt`）。
- Scene pipeline：默认开启；`scene-pipeline.disable` 可关闭。
- Clear fusion：普通 bounded-batch 游戏路径默认开启；`clear-fusion.disable` 优先。
- 有界批处理：由 runner 的 `-BoundedBatch` 或已有 marker 决定；默认 play 路径会写
  `bounded-batch.enable` 吗？以 runner 实际参数为准，不要假设。
- 静态着色器：SELF 编译定义决定目录；runner 会把它 staging 到该目录。

## 5. 日志与截图

每次运行目录包含：

- `process.json`：VPK/SELF/eboot 哈希、Vita3K 哈希、后端、分辨率、截图计划、退出码。
- `config.yml`：本次实际合并的 Vita3K 配置。
- `vita3k.log` / 原生日志：服务、GXM、加载器输出。
- `capture-*.png`：窗口截图（如启用）。
- `backend.json`：OpenGL 初始化证据。

运行日志里常见的周期行：

- `VP_FRAME_BUDGET`：帧时间分解。
- `VP_GXM_STATIC_STATS` / `VP_GXM_BATCH_*`：静态包与批处理统计。
- `VP_GXM_RENDER_RESOLUTION`：实际缩放策略。
- `SVC_STORAGE_SUMMARY` / `SVC_SAVE_*`：存档 I/O。

## 6. 存档安全

- 首次运行前手动备份 `savedata/`。
- 使用 `play.ps1` 的自动备份。
- 不要把真实用户存档提交到 Git。
- 自动化测试应使用独立存档副本，并在结束后恢复原档。
- 强杀进程可能留下半写文件；恢复前先核对备份 manifest 的 SHA256。

## 7. 性能数字怎么读

- 只在同一 Vita3K 构建、同一后端、同一分辨率、同一场景窗口下比较。
- 至少记录 FPS、帧时间中位数、采样区间、彩色/depth draw 数、batch 失败数。
- 单次运行有 ±5–10% 波动；不要把单次差异写成受控加速。
- 模拟器 FPS 不是 PS Vita 真机性能。真机需要独立测试。

## 8. 已知运行问题

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 启动即失败，日志停在 VM 分配 | Vita3K 版本限制 | 换兼容构建；记录版本 |
| 黑屏但进程在跑 | 图形路径/场景未支持 | 查 `gxm-runtime.log` 的拒绝原因 |
| 村庄约 5 FPS | CPU/GXM 准备与等待 | 见 [当前状态](09-status.md)，不是配置错误 |
| 视频约 2.8 FPS | 播放器提交节奏 | 已知问题，非 GPU 瓶颈 |
| 任务画面异常 | 着色器/生产者路径未完全闭环 | 记录日志与截图，提交 issue |
| 存档被覆盖 | 未备份/未恢复 | 从 `local/save-backups/` 恢复 |
