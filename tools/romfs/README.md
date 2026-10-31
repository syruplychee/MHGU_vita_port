# tools/romfs

| 脚本 | 作用 |
|---|---|
| `build_romfs_index.py` | 从解包 RomFS 树生成 `romfs.index`、`romfs.map`、`romfs.json` |
| `build_offline_system_data.py` | 生成空 NgWord2 系统数据 `offline-ngword2.romfs` |

`build_romfs_index.py --tree` 必须是**包含 `nativeNX/` 的那一层**。输出目录会被写入
运行时数据根；`romfs-tree` 由 `scripts/prepare-runtime.ps1` 建立 junction。

格式说明见 [运行时数据](../../docs/reference/runtime-data.md)。
