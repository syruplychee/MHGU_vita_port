# 运行时数据布局

## 1. 根目录

```text
ux0:data/mhgu-vita-port/
```

在 Vita3K 开发环境中对应：

```text
build/vita3k-pref/ux0/data/mhgu-vita-port/
```

## 2. RomFS

| 文件 | 格式 | 说明 |
|---|---|---|
| `romfs.index` | 原 RomFS 头 + 哈希/元数据 | 头 0x50 字节；最后 8 字节 `data_offset` |
| `romfs.map` | `VPRM` v1 | 44 字节头 + 24 字节记录 + 路径字符串 |
| `romfs-tree/` | 目录树 | 实际文件 |
| `romfs.json` | JSON | 生成清单（调试） |

`romfs.map` 记录：

```c
struct { uint64_t image_offset; uint64_t size; uint32_t path_offset; uint32_t reserved; }
```

路径相对 `romfs-tree/`，例如 `nativeNX/arc/...`。加载器会校验：

- map magic/version/reserved
- 记录不重叠、不越界
- `map_data_offset == romfs.index.data_offset`
- `map_image_size == data_offset + tree_bytes`

## 3. 离线系统数据

| 文件 | 说明 |
|---|---|
| `offline-ngword2.romfs` | 自生成空词库 RomFS 镜像；固定 52 个条目 |

## 4. 静态着色器包

| 文件 | magic | 记录 |
|---|---|---|
| `shader-index.bin` | `VPSHIX1\0` | 64 字节头；48/40/16 字节 stage/variant/program |
| `shader-index.json` | JSON | 与二进制等价的审计视图 |
| `shader-resources.bin` | `VPSRES1\0` | 96 字节头；40 字节 stage + 16 字节 texture |
| `shader-resources.manifest.json` | JSON | sidecar 身份与统计 |
| `manifest.json` | JSON | 包级 ABI/计数/哈希 |
| `shaders/<key>.gxp` | `GXP\0` | 已编译程序 |

运行时 ABI 2 支持：

- variant 的 viewport mask 与 input signature
- stage 的 exact SHA256
- program token → vertex/fragment stage
- 资源 sidecar 的纹理槽/类型/usage
- 深度/软件阴影采样策略

详见 [静态着色器包](shader-package.md)。

## 5. 配置与存档

| 路径 | 说明 |
|---|---|
| `render-resolution.txt` | `360p` 或 `540p`（单行，允许尾随空白） |
| `savedata/<id>/system` | 游戏存档主文件 |
| `savedata/<id>/system_backup` | 游戏自身备份 |
| `input-replay.txt` | 输入回放（诊断） |
| `input-live.txt` | 实时输入邮箱（诊断） |

## 6. 日志

| 文件 | 内容 |
|---|---|
| `gxm-runtime.log` | GXM 初始化、静态包、draw/批处理/帧预算 |
| `sdk-svc.log` | SDK 服务、IPC、存储、输入、音频/视频 |
| `audio-sink-progress.log` | 音频输出进度 |
| 运行目录 `process.json` | 本次运行的完整身份/参数 |

## 7. 空间估算

| 项 | 大小 |
|---|---|
| 完整 RomFS | 约 12.3 GB |
| `romfs.index` + `romfs.map` | 数 MB |
| 静态着色器包 | 数 MB（取决于 variant 数） |
| 存档 | 数 MB 到数十 MB |
| VPK | 约 16.5 MB |

开发期用 junction 避免复制 RomFS。真机需要实际复制，确保记忆棒空间。
