# src/platform

Vita 平台服务与图形后端。

| 文件组 | 职责 |
|---|---|
| `guest_svc.c` | SDK 服务/IPC 转接、线程/锁/时间/输入/音频/视频入口 |
| `guest_storage.c` | RomFS index/map/tree 读取 |
| `guest_save.c` | 存档文件系统语义 |
| `gpu_stream.c` | Maxwell/MME 命令解码与状态跟踪 |
| `gxm_runtime.c` | GXM 管线、Scene 批处理、clear 融合、present |
| `gpu_texture.c` | 纹理格式转换、GOB 解平铺、BC 块 |
| `static_shader_index.c` | `VPSHIX1` 索引解析/选择 |
| `gxm_resource_layout.c` | `VPSRES1` 纹理资源 sidecar |
| `gxm_depth*.c` | 深度视图、清除、阴影采样 |
| `audio_*.c` | 音频命令、混音、AudioOut |
| `media_*.c` | 视频解码/呈现 |
| `memory_attributes.c`、`nvmap_table.c` | 内存属性与 nvmap 生命周期 |
| `render_resolution.h` | 360p/540p 缩放策略 |

原则：

- 未实现路径明确报错并记录，不返回假成功。
- 有界缓存/arena；资源 pin/fence 生命周期明确。
- 游戏可见结果（存档、输入、读回）必须真实。
- 静态包缺失/损坏时拒绝，不静默换默认 shader。
