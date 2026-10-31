# tools 目录

工具按**用途**分组。只有 `input/`、`romfs/`、`runtime/` 是日常构建/运行必需；
`shader/` 和 `assets/` 是离线资源流水线；其余脚本是这些流程的支撑/诊断实现。

```text
tools/
├─ input/      输入身份校验、构建期头生成、SDK 契约扫描
├─ romfs/      RomFS 索引/映射、离线系统数据
├─ runtime/    Vita3K 启动、窗口取证、存档/输入辅助
├─ shader/     Maxwell/SPH → GLSL/IR → Cg → GXP、静态包打包/校验
├─ assets/     纹理格式解析/转换、资源清单、静态资源打包
└─ requirements.txt
```

## 常用命令

```powershell
# 输入身份
python tools/input/verify_input.py --exefs game-input/exefs --nacp game-input/control.nacp

# 构建期头（通常由 CMake 自动调用）
python tools/input/prepare_native_loader.py --output build/generated/target_identity.h
python tools/input/prepare_sdk_dso.py --verify
python tools/input/prepare_sdk_svc.py --verify --exefs game-input/exefs
python tools/input/prepare_sdk_timer.py --verify --exefs game-input/exefs
python tools/input/prepare_sdk_barrier.py --verify --exefs game-input/exefs

# RomFS 运行时数据
python tools/romfs/build_romfs_index.py --tree game-input/romfs --output <runtime-data>
python tools/romfs/build_offline_system_data.py --output <runtime-data>/offline-ngword2.romfs

# 静态包校验
python tools/shader/verify_static_package.py <package-dir>
```

## 约定

- 工具默认只读输入，不覆盖原始 dump。
- 生成物写入 `build/`、`local/` 或显式 `--output`，不写入源码目录。
- 需要游戏 shader 库/既有基础包的工具，先读对应 `--help` 和
  [静态着色器包](../docs/reference/shader-package.md)。
- 不要从本目录直接运行来源不明的脚本；所有脚本都应由仓库维护者审阅。
