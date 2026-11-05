# 运行时资源

Vita 端从 `ux0:data/mhgu-vita-port/` 读取所有可变数据。`game/` 下的四模块已经在
VPK 中；其余资源不进入 VPK，也不进入 Git。

## 1. 目录总览

可选点阵字体包独立放在 `font-pixel/`，配套 `pixel-font.enable`；不要覆盖原始树。
完整生成、启用和回滚说明见 [点阵字体结果](19-pixel-fonts.md)。

```text
ux0:data/mhgu-vita-port/
├─ romfs.index              RomFS 头 + 名称哈希 + 元数据
├─ romfs.map                image offset → 文件路径/大小
├─ romfs-tree/              完整 RomFS 树（开发期 junction）
├─ offline-ngword2.romfs    自生成的空词库系统数据
├─ render-resolution.txt    360p 或 540p
├─ static-shaders/          静态着色器源包（冻结）
├─ static-shaders-current/  本次 SELF 编译定义选中的运行包
├─ savedata/                游戏存档
├─ gxm-runtime.log          GXM 日志
└─ sdk-svc.log              SDK 服务日志
```

开发期在 Vita3K 中对应：

```text
build/vita3k-pref/ux0/data/mhgu-vita-port/
```

## 2. RomFS 元数据与树

游戏不读取打包的 12 GB RomFS 镜像，而是读取：

- `romfs.index`：与真实 RomFS 相同的头、名称哈希表和目录/文件元数据。
- `romfs.map`：镜像偏移到主机文件的映射。
- `romfs-tree/`：实际文件树。开发期用 junction 指向 `game-input/romfs`，
  避免复制 12 GB。

生成：

```powershell
.\scripts\prepare-runtime.ps1 `
  -RomFs "D:\MHGU_vita_port\game-input\romfs" `
  -RuntimeRoot "D:\MHGU_vita_port\build\vita3k-pref\ux0\data\mhgu-vita-port"
```

脚本会：

1. 用 `tools/romfs/build_romfs_index.py --tree <RomFs> --output <RuntimeRoot>`
   生成 `romfs.index`、`romfs.map`、`romfs.json`。
2. 创建 `romfs-tree` junction。
3. 生成 `offline-ngword2.romfs`。
4. 复制 `config/render-resolution.txt`。

> `RomFs` 必须是**直接包含 `nativeNX/` 的那一层**。如果指向 `nativeNX` 内部，
> 映射路径会少一层，运行时找不到文件。

如果项目盘受安全策略保护、Python 无法在 `RuntimeRoot` 写入，可把生成过程放到
可写目录，再由 PowerShell 复制：

```powershell
.\scripts\prepare-runtime.ps1 `
  -RomFs "D:\MHGU_vita_port\game-input\romfs" `
  -RuntimeRoot "D:\MHGU_vita_port\build\vita3k-pref\ux0\data\mhgu-vita-port" `
  -StagingRoot "C:\writable\mhgu-runtime-stage"
```

`StagingRoot` 中的中间文件不会被自动删除，确认后可手动清理。

真机上不能用 junction，需要把完整树复制到记忆棒；确保剩余空间足够。

## 3. 离线系统数据

`offline-ngword2.romfs` 是项目自生成的空 NgWord2 词典（4,096 个零字节 gzip 流），
让游戏自己的匹配代码可以运行，但不提供任何屏蔽词。它不包含任天堂固件或词表。

```powershell
python tools/romfs/build_offline_system_data.py --output <RuntimeRoot>/offline-ngword2.romfs
```

## 4. 静态着色器包

Vita 不在运行时编译 Maxwell/Cg。离线包包含：

| 文件 | 格式 | 作用 |
|---|---|---|
| `shader-index.bin` | `VPSHIX1` | stage/variant/program 定长索引 |
| `shader-index.json` | JSON | 可审查的等价索引 |
| `shaders/<key>.gxp` | GXP | 已编译的 Vita 片元/顶点程序 |
| `shader-resources.bin` | `VPSRES1` | 每 stage 的纹理槽/类型/使用掩码 sidecar |
| `shader-resources.manifest.json` | JSON | sidecar 身份与统计 |
| `manifest.json` | JSON | 包级身份、计数、ABI |

格式细节见 [静态着色器包](reference/shader-package.md)。

### 4.1 安装现成包

把维护者提供（且你有权使用）的包放到 `local/`，然后：

```powershell
.\scripts\install-shaders.ps1 `
  -PackageDir "D:\MHGU_vita_port\local\shader-package" `
  -RuntimeRoot "D:\MHGU_vita_port\build\vita3k-pref\ux0\data\mhgu-vita-port"
```

脚本会先用 `tools/shader/verify_static_package.py` 校验：

- manifest 的 ABI 与计数
- `shader-index.bin` SHA256 / CRC
- 每个 variant 的 GXP 大小、SHA256、CRC32
- `shaders/` 中没有未引用文件
- `shader-resources.bin` 与其 manifest 一致

然后把包复制为 `static-shaders/`（源包）。`run_gui_play.ps1` 会把它 staging 到
`static-shaders-current/`。

### 4.2 从锁定输入重建

`tools/shader/` 保留了完整离线流水线，但它依赖：

- SPK shader 库（`unpack/mhgu-shaders` 一类）
- Maxwell/SPH 审计输出与 GLSL 解析结果
- Shacc + SceShaccCg
- 既有基础包与观察到的缺失 stage 记录

当前流程不是“一条命令从零生成全量包”。主线步骤：

1. `audit_static_library.py`：建立 stage/program 清单与 Cg 候选。
2. `compile_static_candidates.py`：编译 GXP。
3. `augment_static_shader_package.py`：把新 stage 加入既有包。
4. `build_resource_layout.py`：生成 `VPSRES1` sidecar。
5. `verify_static_package.py`：完整性校验。

具体参数因输入目录而异，详见 `tools/shader/` 脚本的 `--help` 和旧工作区的 ADR/报告。
不要在没有实际运行证据时宣称“全量包已完成”。

### 4.3 构建期目录名

SELF 在编译时绑定一个运行目录：

```text
VITA_PORT_STATIC_SHADER_PACKAGE_DIR
默认 ux0:data/mhgu-vita-port/static-shaders-current
```

`build/vita/static-shader-package.json` 记录该值。`run_gui_play.ps1` 会检查包目录与
SELF 编译值一致，否则拒绝启动。不要用 `static-shaders` 或 `shaders` 作为编译目录，
它们是冻结源包/legacy 路径。

## 5. 分辨率

`config/render-resolution.txt` 只接受单行 `360p` 或 `540p`（允许尾随空白）：

- `540p`（默认）：1280×720 主画面 → 960×540；最终扫描 960×544。
- `360p`：1280×720 → 640×360。
- 1920×1080 在两种模式下都限制为 960×540。
- 方形、非 16:9、小于 160×90 的图像保持原尺寸。

`prepare-runtime.ps1` 和 `run_gui_play.ps1` 都会把仓库文件复制到运行时目录。
文件缺失时默认 540p；存在但不支持的值会报错并停止 GXM，不会静默回退。

## 6. 存档

存档位于：

```text
ux0:data/mhgu-vita-port/savedata/<program>_<uid>_t<type>_r<rank>_i<index>/
```

游戏同时使用嵌套目录和平面文件。测试/试玩前必须备份：

```powershell
.\scripts\backup-saves.ps1
.\scripts\backup-saves.ps1 -Restore "local/save-backups/<timestamp>" -Replace
```

`play.ps1` 会在启动前自动备份。默认保留新存档；加 `-RestoreAfter` 可在退出后恢复
启动前状态。

## 7. 运行时 marker

`play.enable`、`display-bridge.enable`、`media-video.enable` 等 marker 控制诊断路径。
默认运行由 `run_gui_play.ps1` 自动写入必要的 play/display/media marker，并隔离冲突
诊断 marker。完整列表见 [运行时 marker](reference/markers.md)。

## 8. 不要提交什么

`romfs.index`、`romfs.map`、`romfs-tree/`、`offline-ngword2.romfs`、静态着色器包、
纹理/模型资源、日志、存档都属于本地生成或游戏派生数据，已在 `.gitignore` 中排除。
