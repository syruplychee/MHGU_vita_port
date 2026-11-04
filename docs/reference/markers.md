# 运行时 marker

Marker 是 `ux0:data/mhgu-vita-port/` 下的空文件或短文本文件。运行器会在启动前写入/
隔离部分 marker，因此**不要依赖手工 marker 覆盖 runner 的默认策略**。

## 1. 游戏路径

| Marker | 作用 |
|---|---|
| `engine-full.enable` | 实验：替换公共队列、program／材质资源及状态准备；与 engine-layout／engine-submit-probe 互斥；默认关闭，restart 修正后村庄采样画面恢复，见 ADR 0019–0020 |
| `engine-full-fast.enable` | 实验：启用已核对的纯查询直接计算、减少准备开销；存在时启用完整接管，默认关闭，见 ADR 0022 |
| `engine-full-ab.enable` | 实验：同进程交替运行完整接管的原入口与 C 路径。存在时即使没有 `engine-full.enable` 也启用完整接管；与 engine-layout／engine-submit-probe 互斥；`VITA_PORT_ENGINE_CAPTURE` 诊断构建显式拒绝；见 ADR 0021、0023 |
| `engine-full-mode.txt` | A/B 控制文件（不是 marker）：内容必须是一个 `0`–`6`，允许尾部空白。`0`=原 root，`1`=基础 C，`2`=优化 C，`3`=shared（原 root + 下游共享准备），`4`=deep（原 root + 独立深纹理管线），`5`=deep-recycle（同 4，允许先回收空闲的异尺寸纹理页），`6`=deep-clear（同 5，另允许符合准入的全通道颜色 clear 延迟到消费点并延后同步；depth/stencil、部分通道、资源不匹配及诊断/capture 仍走原同步路径）。仅 `engine-full-ab.enable` 存在时读取；补丁阶段先校验，运行时每 500 ms 轮询，缺失或非法时显式失败，不回退。`0`／`3`／`4`／`5`／`6` 使用同一原游戏 trampoline；只有 `3` 发布 shared 开关，`4`／`5`／`6` 发布纹理管线开关，`5`／`6` 允许空闲页回收，只有 `6` 发布 clear 延迟开关。见 ADR 0025、0026 |
| `texture-pipeline.disable` | 退出当前分支默认启用的深纹理管线。存在 A/B 控制时由 `engine-full-mode.txt` 决定，本 marker 不覆盖。切换在 GPU submission 边界 drain 后应用，不改变 draw 内控制流 |
| `play.enable` | 进入可玩路径；runner 默认写入 |
| `display-bridge.enable` | 启用显示桥 |
| `display-bridge.disable` | 显式关闭显示桥 |
| `gpu-present.enable` | 启用 GPU present |
| `media-decode.enable` | 启用媒体解码 |
| `media-video.enable` | 启用视频播放 |
| `media-native-readback.disable` | 关闭 CPU 视频表面的原生读回复制，恢复原 SDK 按行缓存同步路径；启动时读取，用于对照/回滚 |
| `sdk-lock-fast.disable` | 关闭 SDK 锁轻量入口，继续使用完整上下文桥；锁递归及错误语义不变 |
| `sdk-session-kernel.enable` | Session 全部使用原内核互斥体，供同一 SELF 对照用户态门锁；启动时读取 |

## 2. 渲染策略

`engine-layout.enable` 启动时接管已锁定的游戏公共入口，复用顶点布局的完整准备结果；
当前为显式选择，详见 ADR 0018。`engine-submit-probe.enable` 仅测原入口，默认关闭，
两者不能同时使用。它们都不允许通过跳过 draw 或降低效果来宣称提速。

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
| `frame-cost.enable` | 启用独立的 GXM worker 帧成本分解计时。不存在时不做持续轮询/取时钟；存在时才在完整 decoder-entry 边界读取控制文件。默认关闭，见 ADR 0027 |
| `frame-cost-mode.txt` | `frame-cost.enable` 存在时的控制文件（不是 marker）：内容必须恰好是 `0` 或 `1`，允许尾部空白。`0` 保持同样的边界轮询频率但不建 span；`1` 启用 span。缺失或非法时显式记录 control error 并禁用本进程数据。切换只在 span 栈为空时发生，记录 epoch/mode/time |
| `VP_FRAME_COST_SNAPSHOT_BEGIN/ROW/END` | 仅 mode 1：每 32 个 present 输出同一身份的完整累计快照；最终 schema 3 为 22 类，另有同端点 GXM/纹理细分。关闭时补最后闭合快照。缺行、错误、倒退、self/root 不对账均拒绝；见 ADR 0027 |
| `VP_FRAME_COST_POINT` / `VP_FRAME_COST_EPOCH` / `VP_FRAME_COST_CONTROL` | mode 0/1 切换、epoch 端点、marker 删除和控制错误的显式记录；不包括成本 span |
| `VP_FRAME_COST_ROOT` | 闭合 worker 根调用的起止 journal；区间和必须等于同端点 root 累计差。用于和原游戏区间求并集，不能跨线程相加 |
| `frame-resource-audit.enable` | 默认关；与 frame-cost 一起启用，每个 mode-1 epoch 的前 16 个 present 范围记录纹理完整 key/长度/真实查询结果，不写像素。不纳入正式性能窗口；首尾不完整帧由分析器排除 |
| `game-cost.enable` | 默认关；安装四个原游戏 A32 函数观测钩子，保留原函数，记录嵌套与平台桥墙钟。入口、原字节、范围和 ABI 见 `config/patch-plans/game-cost.json`、ADR 0028 |
| `VP_GPU_WAIT` / `VP_GPU_WAIT_TOTAL` | play-perf 下每 32 个 present 输出完整 28 原因累计快照；记录真实通知等待、poll 命中、失败、阻塞时间及粗直方图，与原全局通知计时对账。mode 6 的 clear-materialize 等待单列一行。用 `analyze_gpu_wait.py` 做完整快照端点差分；不是 GPU 硬件执行时间 |
| `draw-trace.enable` | draw 跟踪 |
| `vtx-dump.enable` | 顶点 dump |
| `b34-passdump.enable` | pass 级 dump；存在期间强制同步 scene，marker 消失前保持原行为 |
| `capture-live.enable` / `capture-request.txt` | 实时截图邮箱；`inputs!=0` 的 surface 输入 capture 期间强制同步，只拍 present（`inputs=0`）保持异步流水线，capture 完成后恢复 |
| `shader-word-dump.disable` | 关闭 shader word dump（runner 默认写） |
| `texture-asset-dump.enable` | 纹理资产 dump |
| `media-instrument.enable` | 媒体详细计时 |
| `VP_TEX_POOL_STATS` / `VP_GXM_TEXTURE_PIPELINE_STATS` | deep 管线累计计数日志（既有 512 request／256 draw 节奏）：region 申请/复用、pressure、decode family、direct bind/bytes、retain/release/quarantine、实际 memcpy 字节、deferred scene、notification poll/real wait、combined drain、mode transition。region 模式的 arena 碎片项为 0，不能据此判断系统碎片；普通计时包含嵌套项 |
| `VP_GXM_CLEAR_DEFER_STATS` | mode 6 累计计数，在既有 `static_log_stats` 节奏输出：try／accept／fallback／scene submit／coalesce／materialize 调用与失败／in-scene 消费／仍带 async 标签的 surface 数。不新增逐 clear 日志；`tries` 与 accept+fallback+submit_fail 对账，失败含真实错误传播 |

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
| `media-color-probe.enable` | 启用原视频颜色转换计时探针；仅接受锁定 subsdk0 身份，默认关闭，会临时改写 4 处原指令 |
| `native-movie.enable` | 启用框架级原生播片；从 ROM 树读取用户自己的 MP4，复用 SceAvcdec，原生呈现视频。默认关闭；当前无音频。移除后重启回到原 guest 管线 |
| `native-movie.limit-ms` | 诊断：文件内十进制毫秒数，原生播放在该时长后停止并走游戏停止命令；只用于有界测试，不作为验收结果 |
| `thread-probe.enable` | 诊断：记录 guest ThreadType 的 user entry/arg/name、uMovie vtable 和 movie 控制地址；只读，默认关闭 |
| `movie-force-stop.enable` | 诊断：首次解码 5 秒后向游戏 uMovie 命令字写入 8，用于单独验证停止路径；不用于正式播片 |

## 6. 优先级

- `clear-fusion.disable` > `clear-fusion.enable` > 默认策略。
- `scene-pipeline.disable` > 默认开启。
- `bounded-batch.disable`/`enable` 同时存在时以源码实现为准；runner 会快照并恢复。
- 手工 marker 可能与 runner 冲突；正式验收使用 runner 参数，不手写 marker。

## 7. 安全

- 不要在真实用户存档环境启用破坏性诊断。
- 诊断 marker 可能生成大日志/大 dump；运行前确认磁盘空间。
- 运行结束后清理诊断 marker，避免影响下一次运行。
