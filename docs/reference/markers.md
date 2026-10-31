# 运行时 marker

Marker 是 `ux0:data/mhgu-vita-port/` 下的空文件或短文本文件。运行器会在启动前写入/
隔离部分 marker，因此**不要依赖手工 marker 覆盖 runner 的默认策略**。

## 1. 游戏路径

| Marker | 作用 |
|---|---|
| `play.enable` | 进入可玩路径；runner 默认写入 |
| `display-bridge.enable` | 启用显示桥 |
| `display-bridge.disable` | 显式关闭显示桥 |
| `gpu-present.enable` | 启用 GPU present |
| `media-decode.enable` | 启用媒体解码 |
| `media-video.enable` | 启用视频播放 |

## 2. 渲染策略

| Marker | 默认 | 说明 |
|---|---|---|
| `scene-pipeline.enable` | 默认开启 | 旧显式开关；B116 起普通 batch 默认使用 |
| `scene-pipeline.disable` | — | 显式关闭 Scene 流水线 |
| `clear-fusion.enable` | 视路径 | 显式开启 clear 融合 |
| `clear-fusion.disable` | — | **优先**关闭；不会被 runner 自动移动/删除 |
| `bounded-batch.enable` | 显式开启 | 有界批处理；runner `-BoundedBatch` 写入 |
| `bounded-batch.disable` | — | 显式关闭 |
| `upload-cache-32m.enable` | 关闭 | 32 MiB 上传缓存候选 |
| `cdram-attachments.enable` | 关闭 | 颜色/深度附件放 CDRAM |
| `texture-identity-skip-bytes.enable` | 关闭 | 跳过纹理逐字节比较（诊断/极端优化） |
| `gpu-present-bottom-origin.enable` | 关闭 | present 原点诊断 |

## 3. 性能与日志

| Marker | 作用 |
|---|---|
| `play-perf.enable` | 启用逐帧/服务计时；关闭时计时字段为 0 |
| `draw-trace.enable` | draw 跟踪 |
| `vtx-dump.enable` | 顶点 dump |
| `b34-passdump.enable` | pass 级 dump |
| `shader-word-dump.disable` | 关闭 shader word dump（runner 默认写） |
| `texture-asset-dump.enable` | 纹理资产 dump |
| `media-instrument.enable` | 媒体详细计时 |

## 4. 输入

| Marker/文件 | 作用 |
|---|---|
| `input-live.enable` | 启用实时输入邮箱 |
| `input-live.txt` | 邮箱内容：`buttons lx ly rx ry` |
| `input-replay.txt` | 回放文件；runner `-Replay` 复制到此处 |
| `input-capture.enable` | 记录实际写入 npad 的变化到 `input-capture.csv` |
| `hid-helper-probe.enable` | HID helper 诊断 |

## 5. 诊断/测试

| Marker | 作用 |
|---|---|
| `m2-frame.enable` | 单帧探针 |
| `m3-drawdump.enable` | draw dump |
| `m3-viewport.enable` | viewport 诊断 |
| `scene-probe.enable` | Scene 探针 |
| `f8-probe.enable` | 模型/场景探针 |
| `f8-state-override.enable` | F8 状态覆盖 |
| `gxm-test.enable` | GXM 自测 |
| `batch-selftest.enable` | 批处理自测 |
| `swkbd-selftest.enable` | 软键盘自测 |
| `audio-probe.enable` / `audio-probe-monitor.enable` | 音频探针 |
| `audio-command-dump.enable` | 音频命令 dump |
| `capture-request.txt` / `capture-live.enable` | 实时截图邮箱 |
| `applet-exit` / `applet-exit-delay-ms` | 自动退出（诊断） |

## 6. 优先级

- `clear-fusion.disable` > `clear-fusion.enable` > 默认策略。
- `scene-pipeline.disable` > 默认开启。
- `bounded-batch.disable`/`enable` 同时存在时以源码实现为准；runner 会快照并恢复。
- 手工 marker 可能与 runner 冲突；正式验收使用 runner 参数，不手写 marker。

## 7. 安全

- 不要在真实用户存档环境启用破坏性诊断。
- 诊断 marker 可能生成大日志/大 dump；运行前确认磁盘空间。
- 运行结束后清理诊断 marker，避免影响下一次运行。
