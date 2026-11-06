# 0027：可关闭、可对账的整帧成本计时

日期：2026-11-05。状态：L0/L1/L2 与有限村庄 L3。数字、采用判断和未验证范围统一见
[主要成本审计](../16-frame-cost-audit.md)。本 ADR 定义测量，不定义提速。

## 为什么增加这套计时

旧 `VP_FRAME_BUDGET` 的 command dispatch 包含 MME、draw、纹理和 GXM；原游戏函数
也可能同步进入后端。把这些包含时间相加，会把同一份成本算几遍。原来的 residual
还把所有未插点工作和调度混在一起，不能叫原游戏逻辑耗时。

保留旧指标，新增单 GXM worker 的有界嵌套计时，再与 ADR 0028 的原函数区间对账。
不删等待、不改资源生命周期、不跳过 draw，也不改变原错误返回。

## 纯计时规则

`frame_cost_profile.h/.c` 是可宿主测试的纯逻辑：调用者传入微秒时间，有界栈 32 层。
每类别保存 calls、inclusive、self、max、root_calls、root_inclusive，均为 uint64。

- inclusive 含子调用；self 只扣直接子调用，递归类别也逐层扣除。
- root 只统计从空栈开始的调用，互不重叠；所有 self 的和必须等于 root 的和。
- 时间为 0 是关闭哨兵；关闭时不建 span、不取 span 时钟。
- overflow、underflow、mismatch、clock_regression、missing_time、incomplete_snapshot
  显式累计，不能清零当作通过。begin 失败不生成 end；分析器拒绝有错误的窗口。
- 公共 span 入口只接受 GXM worker。原线程、音频等时间不能作为另一个 worker 累计。

schema 1 有 14 类，schema 2 有 19 类，最终 schema 3 有 22 类。分析器保留对三种实测
版本的严格读取；同一窗口不允许混 schema。

| 类别 | 源码范围 | 嵌套关系/含义 |
|---|---|---|
| entry | `gpu_decode_on_worker` | 完整 decoder entry |
| dispatch | `gpu_dispatch_entry_commands` | 一次 entry 的整个分派循环，不逐 method 计时 |
| mme | `vp_mme_execute` 周围 | 每个 macro，不逐指令；包括回调的 draw |
| draw-submit / clear-submit | `frame_submit_draw` / `frame_submit_clear` | 外 wrapper 覆盖全部提前返回 |
| draw-state | 绘制请求初始化和状态 | draw-submit 子阶段 |
| draw-index-window | 索引范围与顶点窗口 | 包括真实索引扫描和映射 |
| draw-cbuf-resolve | 两 stage 的 cbuf 地址 | 不含后续纹理字节比较 |
| draw-resources | shader/纹理描述及资源准备 | 包含 lookup、decode、store、tail 等子类别 |
| draw-final | 请求调用与最后清理 | 包含 gxm-request、GXM 和 worker-tail |
| tex-lookup | legacy/deep 的真实查询 | 全键及 raw 字节比较，没有用 hash 代替 |
| tex-store | 选槽/分配/pressure 重试 | pressure 时包含真实 flush/wait 子项 |
| tex-decode | 真实转换/反平铺 | 既有格式与失败语义不变 |
| tex-tail | swizzle/raw-copy/commit | miss-only 保存 |
| gxm-request | `guest_gxm_request` | 只包原调用，提前返回也闭合 |
| gxm-draw | `frame_draw_internal` | GXM 准备/编码/绘制 API，包括失败 |
| gxm-clear / gxm-flush | 原 clear/flush wrapper | 不改变原完成条件 |
| wait-notify / wait-finish | 真实 SDK wait 周围 | API 阻塞，绝不是 GPU 硬件执行时间 |
| worker-tail | draw 返回后到 worker 统计结束 | 包含此处日志子项 |
| log | 工作内部的 glog/slog | 计格式化和写入；从父类别扣除，不重复相加 |

draw-submit 用小的 phase cursor 切换上述五个子阶段，外 wrapper 在任何返回后关闭
当前子阶段。没有每 method 时钟，没有逐 draw 性能日志。root journal 每 decoder
entry/外部根调用一行，用于区间对账；新的统计快照和 root journal 本身在所有 span
之外，因此其开销留在 residual，并通过 0/1 对照观察。

## 开关与同进程开销对照

`frame-cost.enable` 默认关闭。首次完整 worker entry 检查一次；不存在后不持续轮询，
也不取 span 时钟。存在时，worker 空栈边界最多每 500 ms 检查 marker 和控制文件
`frame-cost-mode.txt`，内容必须恰为 0/1，允许 ASCII 尾空白：

- 0：保留同样的边界时钟和轮询频率，不建 span。
- 1：开始新 epoch、清零该 epoch 的 span 计数，启用计时。
- 1→0：先输出最后一个闭合快照，再关闭；跨模式的原调用不会被切一半。
- 缺失/非法控制：显式记录 control error、禁用本进程数据，不静默当作某个有效模式。

缺失时不把错误当 0；stack 非空不能切换。开关独立于 engine-full mode，避免把
“计时开销”与“优化路径变化”混在一起。`play-perf` 是另一套旧计时/日志策略；本轮
分别带/不带它运行，不能预先认为该 marker 只影响时间读取。

## 同端点快照与 journal

每 32 个 present，BEGIN、固定行集、END 全部使用同一个 schema/seq/epoch/mode/
t_us/frames。缺行、重复行、顺序错、累计倒退、错误计数、未闭合栈均拒绝。

schema 2/3 还要求同身份的两行 `VP_FRAME_COST_GXM`（colour、depth-only）和一行
`VP_FRAME_COST_TEXTURE`。GXM 细分沿用既有成功 batch 计数，包含 prep、begin、encode、
draw、vertex/index/texture、pair、uniform、resolve 等；有嵌套，不能重复相加。它不
覆盖失败/legacy draw，差额仍留在 gxm-draw。纹理各格式的旧计时在 play-perf 关闭时
为 0，此时不能把 0 解释成没有转换；新的 tex-decode span 仍工作。

`VP_FRAME_COST_ROOT` 为 schema 2/3 的每个闭合根调用记录完整起止区间。区间和必须
等于对应端点的 root 累计差；原游戏/worker 的时间并集由 ADR 0028 的工具计算。
不允许直接把其他时点的 `VP_GXM_BATCH_ENCODE_PERF` 插进这个闭合表。

## 纹理未命中取证

可选的 `vp_texture_pool_lookup_trace` 仍执行同一个真实 pool 查询、逐字节比较、raw
退休和 pin 操作，只额外输出：有 raw 的匹配 key、只有 GPU 版本的 key、比较次数、
请求比较长度和发现内容变化的次数。旧 API 仍可传空 trace，查询决策不读诊断结果。
比较长度是请求长度，不能当作 early mismatch 前真正读过的字节数。

在 profile 活跃期间累计 miss_changed/miss_absent/miss_gpu_only/compare_bytes/
compare_calls，和纹理格式字节数在同一端点读取。三种 miss 分类必须对账；absent
不自动等于“源像素不变”，因为被淘汰后已没有旧 raw 供比较。

另有默认关闭的 `frame-resource-audit.enable`，只能与 frame-cost 一起启用。每个
mode-1 epoch 只记录前 16 个 present 范围内的请求元数据，不写像素、不另做 hash：
frame、完整 key、raw/output 长度、hit 和真实比较结果。正式窗口排除这些捕获段。
`analyze_frame_resources.py` 排除每段首尾不完整帧，按完整 key + raw 长度分组。
同 key 重复请求只能证明身份重复，不能证明被淘汰后的内容未变化。

## 可重复分析

```powershell
python tools/engine/analyze_frame_cost.py --gxm-log <run>/gxm-runtime.log `
  --window <start_us>:<end_us> --process-evidence <run>/process-lifecycle.json `
  --save-protection <run>/save-protection.json --output <report-dir>
python tools/engine/analyze_game_cost.py --run <run> `
  --cost-report <report-dir>/report.json --output <run>/game-analysis.json
python tools/engine/analyze_frame_resources.py --sdk-log <run>/sdk-dso.log `
  --output <run>/resource-analysis.json
```

所有累计用完整端点差；max 只是 epoch 端点上界，不冒称窗口最大值。residual 保留
符号，负数不能 clamp 成 0。过程要求真实 exit 0、exited、未强停、无可疑崩溃、PID
身份核验；存档与 marker 必须恢复且 verified，窗口至少 128 帧。缺证据不能 performance_pass。
这只是测量证据门，speedup_claim 恒为 false。整帧分布另用既有严格 histogram 工具，
每段至少 128 帧；同进程 0/1 只是描述性开销对照，含自然动画/调度差异。

输入日志、工具和证据文件均记录 SHA256。原始日志、图像和游戏派生物只在 ignored
运行目录。宿主测试直接编译生产纯计时/纹理池逻辑，检查递归/self、提前返回、溢出、
错误、严格控制、完整快照/细分、重叠与纹理版本持有。真机及全玩法不在本 ADR 验收中。
