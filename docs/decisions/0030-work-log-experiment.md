# 0030：仅减少正常工作日志的对照

- 日期：2026-11-05
- 状态：实验实现，默认关闭；L3 性能结果待实际运行

## 改动

`engine-full-mode.txt` 扩为单个 ASCII 0–9；9 为 `deep-quiet`。它使用与 mode 4
相同的原 root trampoline、64 MiB 统一纹理池，不启用 shared、idle resize、deferred
clear 或 cached raw。只有明确列出的正常日志在格式化之前被省略，业务操作和计数不变。
普通启动和非法控制错误路径清除该开关；所有旧模式语义不变。

GXM 省略项：成功 BATCH_ENCODE、成功 BATCH_COMMIT，以及 SCENE_CLASS、BLEND_CENSUS、
BATCH_ENCODE_PERF、API_PERF、XFER、STRIP_JOIN、INDEX_FAST、UPLOAD_CACHE hits、
STATE_CACHE sets 的正常性能/缓存信息。UPLOAD_CACHE 的初始化/释放记录仍保留。

SVC 省略项：`VP_GXM_FRAME_SUBMIT/RECORD` 仅在 rc==0 时；
`VP_GXM_BATCH_SUBMIT_FLUSH` 仅在已经通过 flush 成功分支时。失败分支始终用原 logger。

使用显式 `glog_work` / `slog_work_success` 调用点，不按日志文本黑名单判断安全。
所有参数求值、格式化及写入均在正常日志策略放行后执行；被省略记录不建立 LOG 计时
span。已有计数递增在调用前，未放入可跳过的参数表达式。

## 保留项

错误、断言、拒绝、REFUSE_TOP、STAGE9、poison/quarantine、direct_fail、输入身份、
补丁 guard、生命周期、控制错误，以及 FRAME_COST/ROOT、FRAME_INTERVALS/BUCKETS、
TEX_MEMORY、ENGINE_PREPARE、资源审计等测量证据，全部保留。未改变日志 flush、锁、
原 play-perf 采样、绘制、资源校验、GPU 等待和任何失败传播。

## 观测与验证

每 32 个 present 的同一成本端点输出 `VP_WORK_LOG_STATS`，包括 reduce 开关和
GXM/SVC 正常记录 emitted/suppressed 累计值。32 位原子保证 SVC 并发计数；不引入
ARM 64 位原子链接依赖。模式 4 的 suppressed 差必须为 0，9 必须真的增长；两者
均须保持 64 MiB、相同资源策略。

`work_log` 宿主测试直接使用生产策略检查切换、实际放行/省略计数和 mode 9 隔离；
原 GXM 提取逻辑测试保留，只为新的正常日志宏提供既有日志 stub。

性能窗口仍须至少 128 帧、完整直方图、控制阶段一致、无 GPU/SDK fatal、正常进程退出
和原安装/存档/marker 恢复。未关闭细分探针的结果只代表相同诊断条件，不直接推广为
无探针生产性能或真机收益。
