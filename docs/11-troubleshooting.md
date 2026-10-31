# 故障排查

## 1. 构建环境

### `Set VITASDK ...` / `vita.toolchain.cmake` 缺失

设置 `VITASDK` 到 SDK 根目录，确认：

```powershell
Test-Path "$env:VITASDK\share\vita.toolchain.cmake"
Test-Path "$env:VITASDK\bin\arm-vita-eabi-gcc.exe"
```

### `No module named lz4`

```powershell
python -m pip install -r tools/requirements.txt
python -c "import lz4.block; print(lz4.__version__)"
```

如果 `python` 打开 Microsoft Store，请设置 `MHGU_PYTHON` 到真实解释器。

### 系统临时目录权限拒绝

Windows 上 `%TEMP%` 可能不可写，导致 GCC/CMake/Ninja 报：

```text
Cannot create temporary file in C:\Users\...\AppData\Local\Temp\: Permission denied
```

`scripts/build.ps1` 会把 `TMP`/`TEMP` 指到 `build/vita/tmp`。手动构建时：

```powershell
$env:TMP = "$PWD\build\vita\tmp"
$env:TEMP = $env:TMP
```

### CMake `pkgRedirects` 或 `Permission denied`

CMake 4.4.x 在部分非系统卷上创建构建目录会失败。处理：

1. 改用 CMake 4.3.x；
2. 或把构建目录放到系统盘：

```powershell
.\scripts\build.ps1 -BuildDir "C:\builds\mhgu-vita"
```

### `ninja: error: WriteFile(.ninja_lock): Unable to create file. Permission denied`

某些受管控的 Windows 机器只允许特定进程写数据盘。表现是 PowerShell/cmd 能写，
但 GCC/Python/Ninja 被拒绝。处理：

- 把 `-BuildDir` 指到系统盘可写目录；
- 或让 IT 给项目目录/工具链加白名单；
- 生成运行时元数据时可用 `prepare-runtime.ps1 -StagingRoot <可写目录>`；
- 不要通过复制游戏文件到 C 盘来绕过身份检查。

这不是项目源码问题；Linux CI 和其他开发机通常不受影响。

### `undefined reference to vp_bridge_enter` / `_ZTIi`

`.S` 汇编没有参与编译。确认 `CMakeLists.txt` 中有 `enable_language(ASM)`，并且
`src/bridge/armv7/*.S` 在目标源列表中。不要删除这些文件。

### `VITA_PORT_EXEFS is missing ...`

检查 ExeFS 是否包含：

```text
rtld  main  subsdk0  sdk  main.npdm
```

注意 `main.npdm` 用于校验，不打包进 VPK。

### `VITA_PORT_NATIVE_TITLE_ID must be nine ...`

Title ID 必须是 9 位大写字母/数字，例如 `MHGUVP018`。

## 2. 输入与身份

### `size mismatch` / `SHA256 mismatch`

输入不是锁定的完整 1.4.0，或文件被修改。重新解包/合成，不要改 `target-1.4.0.json`。

### `display version mismatch`

`control.nacp` 不是 1.4.0。使用更新包的 NACP。

### RomFS 找不到文件 / 路径少一层

`MHGU_ROMFS` 必须是**包含 `nativeNX/` 的那一层**。运行：

```powershell
Test-Path "$env:MHGU_ROMFS\nativeNX"
```

必须为 `True`。

### 中文缺失

合成 RomFS 时漏了 `chS/chT`。检查：

```powershell
Test-Path "$env:MHGU_ROMFS\nativeNX\chS"
Test-Path "$env:MHGU_ROMFS\nativeNX\chT"
```

目录存在只是必要条件；仍需实际界面/对话验证。

## 3. 静态着色器包

### `Shader bundle missing`

`ux0:data/mhgu-vita-port/static-shaders/shader-index.bin` 不存在。安装包：

```powershell
.\scripts\install-shaders.ps1 -PackageDir <dir>
```

或确认 `run_gui_play.ps1 -StaticShaderSource` 指向正确目录。

### `Static shader source contains an unexpected file`

包目录里混入了 manifest 之外的文件。重新复制干净包；不要让编辑器生成 `.bak`、
`.DS_Store`、`Thumbs.db`。

### `VPSHIX1 body CRC mismatch` / GXP SHA mismatch

包损坏或被修改。用 `tools/shader/verify_static_package.py` 重新校验，不要手改二进制。

### `Compiled static shader package must not be the frozen/legacy bank`

`VITA_PORT_STATIC_SHADER_PACKAGE_DIR` 不能是 `static-shaders` 或 `shaders`。使用
`static-shaders-current` 一类独立目录。

## 4. Vita3K

### 启动即失败，日志停在 VM 分配

Vita3K 版本限制。换一个兼容构建，并在 `process.json`/报告中记录版本和 SHA256。
不要修改加载器布局来“适配”某个模拟器，除非有 ADR 和真机证据。

### 窗口全白/全黑

- 先确认 `backend-renderer: OpenGL` 且日志有 `[renderer::gl::create]`。
- 检查截图是否为模拟器主窗口而非游戏窗口。
- 查 `gxm-runtime.log` 是否有 `ENTRY_REJECT`、`BATCH_FAIL`、`SHADER_*` 拒绝。
- 单帧截图不能证明动画正常；需要连续时间点。

### 黑屏但 FPS 在跑

可能是任务/场景图形路径未闭环，不一定是崩溃。收集：

- 场景进入时间
- `VP_GXM_*` 统计
- 首次拒绝的 stage/纹理
- 实际窗口截图

### 视频很慢

已知问题。先确认日志 `VP_MEDIA_VIDEO_DECODE_TIME` / `COMPOSITE_TIME` 是否为毫秒级；
若是，瓶颈不在解码，而在播放器提交/等待。

## 5. 存档

### 存档丢失/被覆盖

从 `local/save-backups/<timestamp>/` 恢复：

```powershell
.\scripts\backup-saves.ps1 -Restore "local/save-backups/<timestamp>" -Replace
```

恢复前脚本会校验 manifest 中每个文件的 SHA256。若当前 `savedata/` 已存在，会先改名
为 `savedata.pre-restore-<timestamp>`。

### 存档写入一半

强杀进程可能导致。检查 `savedata/` 下是否有临时文件；从备份恢复，不要手动拼接。

## 6. 测试

### 宿主测试 Debug 通过、Release 崩溃

旧测试依赖 `assert` 的副作用。CMake 已用 `/UNDEBUG`/`-UNDEBUG` 保留断言；如果仍
失败，优先把测试改为显式检查，而不是关闭断言。

### Linux 测试未构建

`audio_auto_catchup`、`audio_wave_progress`、`guest_storage`、`log_sink` 使用 POSIX
设施，只在 `UNIX` 下构建。Windows 上跳过是预期行为。

### CTest 找不到 exe

多配置生成器（Visual Studio）把 exe 放在 `Release/` 或 `Debug/` 下。用
`ctest -C <Config>` 或 `scripts/host-tests.ps1`。

## 7. 日志与诊断

- 运行目录：`build/runs/play-<timestamp>/`
- 原生日志：`build/vita3k-pref/ux0/data/mhgu-vita-port/gxm-runtime.log`
- 服务日志：`sdk-svc.log`
- 性能：`play-perf.enable` 后看 `VP_FRAME_BUDGET`
- 着色器：`VP_GXM_STATIC_*`
- 批处理：`VP_GXM_BATCH_*`
- 存档：`SVC_STORAGE_*` / `SVC_SAVE_*`

日志过大时，只保留关键行和相关时间窗口；不要把 1 GB 日志提交到 issue。
