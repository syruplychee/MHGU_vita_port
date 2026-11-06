# 0021：公共渲染准备的同进程 A/B 计时框架

日期：2026-11-04。状态：框架和受保护的同进程运行已验证；优化与实测见
[ADR 0022](0022-engine-prepare-shared-queries.md)。

## 背景

[ADR 0019](0019-engine-full-prepare.md) 的完整接管在修正 restart 参数后画面恢复正常，
但两次独立运行的原路径约 181.0 ms/帧、当前 C 路径约 191.6 ms/帧。两次运行来自不同进程，
无法区分候选本身的成本、场景差异、模拟器调度、热状态和测量窗口差异，因此不能据此判断
提速或回退。需要同一个进程内交替选择“原 root”与“当前 C”，并把每次完整 root 调用作为
样本记录；A/B 只是测量框架，不是性能结论。

## 决定

### 1. marker 与互斥

- 新 marker `ux0:data/mhgu-vita-port/engine-full-ab.enable`。
- 该 marker 存在时启用完整接管，即使 `engine-full.enable` 不存在；两者同时存在时按 A/B
  处理。普通 `engine-full.enable` 路径和默认关闭路径保持原行为。
- A/B 与 `engine-layout.enable`、`engine-submit-probe.enable` 互斥；检测到组合时在补丁
  阶段显式拒绝，不执行候选。
- `VITA_PORT_ENGINE_CAPTURE` 诊断构建与 A/B 显式互斥。采集构建会在固定调用次数后停机，
  若允许组合会把诊断停机混入测量；拒绝信息为
  `ENGINE_FULL_AB_REJECT reason=engine-capture-build`。普通采集构建不受影响。

### 2. 原 root 可调用 trampoline

原 root 入口仍是 `main+0xBBE9C8`，补丁后为分支到追加 island 的 8 字节 veneer：

```text
e51ff004            ldr pc, [pc, #-4]
<vp_engine_full_entry>
```

A/B 模式在 veneer 之后再追加 12 字节 trampoline：

```text
e92d4ff0            push {r4-r11, lr}    ; 重放被替换的首条指令
e51ff004            ldr pc, [pc, #-4]
<main_base + 0xBBE9CC>                  ; 回到原函数体内继续执行
```

这里**只重放首条 PUSH**，不复制 7004 字节函数体。原函数内部所有 PC 相对指令仍在原地址
执行。trampoline 使用绝对字跳转，因此不需要额外分支可达性；根入口到 veneer 的 A32 B
仍按 ±32 MiB 检查。island 申请范围固定为 20 字节（8 字节 veneer + 12 字节 trampoline），
写前检查 4 字节对齐、`island_offset + island_bytes + request <= capacity` 和根分支可达，
并记录：

- 补丁前 `main+0xBBE9C8` 的前 4 个 word；
- veneer 与 trampoline 的绝对地址、范围、三个 trampoline word 和目标 `0xBBE9CC`；
- island 起止、申请字节数和模块容量。

原有完整函数 SHA-256 门（7004 字节）和另外三个准备函数的 SHA 门全部保留；任何字节
不匹配仍拒绝写入。

### 3. A/B wrapper 与 bridge ABI

`vp_engine_full_entry` 读取全局 `vp_engine_full_profile_enabled`：

- 0：保持原有单路径快速入口 `push {r4,lr}; bl vp_engine_full_run; ...`；
- 1：进入新的 A/B wrapper。

wrapper 的栈与寄存器约定：

```text
push {r4, lr}                    ; 8 字节，保持 AAPCS 的 8 字节 SP 对齐
bl  vp_engine_full_ab_enter      ; host bridge service 0x253
blx ip                           ; 执行 C 或 trampoline
bl  vp_engine_full_ab_leave      ; host bridge service 0x254
pop {r4, lr}
bx  lr
```

enter/leave 都使用既有 `VP_HOST_BRIDGE_FUNCTION` 宏，因此保存并恢复完整 GPR、APSR、
FPSCR 与 d0-d31；原 caller 的 r4-r11、SP、返回地址和浮点状态不被破坏。enter 处理器：

- 保留 `r0 = context`；
- 把本次调用选中的目标地址写入 `f->regs[12]`（ip）；
- 目标为当前 C 函数 `vp_engine_full_run` 或上面 12 字节 trampoline。

native C 目标由当前工具链生成为 Thumb，函数指针保留 bit0=1；trampoline 为 ARM
（bit0=0）。`blx ip` 依据该位切换指令集，两条目标都不需要额外 veneer。

leave 处理器读取时间并结束计时。**只有 native C 路径**检查返回的 r0：非零时记录
`ENGINE_FULL_FATAL`、输出准备统计并返回 `VP_BRIDGE_ARGUMENT`。原 root 的调用者不消费
返回值，根函数可以返回任意 r0，因此 original 路径绝不把 r0 当错误。成功路径恢复
caller LR 后 `bx lr`，保持原 caller ABI。

### 4. 控制文件与模式切换

控制文件：`ux0:data/mhgu-vita-port/engine-full-mode.txt`。

- 内容必须恰好是一个 `0`、`1`、`2` 或 `3`，允许尾部空白（空格、制表符、换行、CR、VT、FF）；
  其他内容非法。
- `0` = 原 root，`1` = 基础 C，`2` = 优化 C（ADR 0022），`3` = shared
  （ADR 0023：与 `0` 相同的原 root trampoline，只启用下游共享准备）。
- A/B 启用时缺失或非法会显式失败，不回退到任一模式。普通非 A/B 运行不读取该文件。
  补丁写入前先校验一次，运行时每次轮询再校验；两处都遵循同一格式。
- 轮询在 host 侧、Session 锁下执行，最多每 500000 µs 一次；文件 I/O 在
  `sceKernelGetProcessTimeWide` 取 root 起始时间**之前**完成，因此轮询成本计入帧但不计入
  `elapsed_us`。两条路径承受相同频率的轮询。
- 模式只在完整 root 进入时选择；已进入的调用在 leave 时使用自己保存的 mode。模式变化
  记录 `ENGINE_PREPARE_PHASE mode=... t_us=...`。

### 5. 每 root 时间记录

Session 按 executor 保存 64 个槽：主线程 slot 0，worker 槽 1..63。每个槽保存进入时间、
context、command、起始 command write、mode 和 active。同一槽嵌套进入、未进入就离开都
显式报错，不覆盖外层记录。

leave 开头读取结束时间；`command` 取进入时 `context+0x94` 的值，
起始/结束 write 取 `command+0xC`。编码字节数为 `after - before`，仅当差值非负、小于 1 MiB 且 command+0x28 的 block base 未改变时视为连续区间；否则 `growth=1` 且 `encoded_bytes=0`，**明确标注换块，不把增长
当零字节**。`presents` 取 `Session.svc` 中 `vi:obj` BufferQueue 的累计
`binder_presents`；分析器只用端点差，绝不把累计值当区间计数。

每次完整 root 一行：

```text
ENGINE_PREPARE_TIME mode=original|native thread=... context=... entry_us=...
  t_us=... elapsed_us=... encoded_bytes=... growth=0|1 presents=...
```

时间均为游戏进程时钟 `sceKernelGetProcessTimeWide` 微秒。框架产生
`original`／`native`／`fast`／`shared`，每段固定选择一条完整 root 路径；`shared`
与 `original` 使用同一原 root，只额外启用下游共享准备（ADR 0023）。

### 6. 分析器与判定边界

`tools/engine/analyze_prepare_ab.py` 为纯标准库 Python，输入：

```text
--sdk-log <sdk-dso.log>       ENGINE_PREPARE_TIME 样本
--gxm-log <gxm-runtime.log>   VP_GXM_PERF_WINDOW kind=colour 端点
--phases <phases.json>        [{"mode","start_us","end_us"}, ...]
--output <新目录>             写入 report.json
--warmup-seconds 5.0          相位起始 warmup
```

要求至少四个相位并按 A/B/A/B 交替；相位时间必须递增且不重叠。相位开始后
`warmup_seconds` 内的样本排除；边界外样本忽略；进入与离开落在不同相位、或跨出相位
范围的样本明确报错。warmup 之后样本的 mode 必须与相位声明一致。

present 区间使用 GXM `VP_GXM_PERF_WINDOW kind=colour` 的 `(t_us, presents)` 端点差；
不可用时使用 SDK 侧 `ENGINE_PREPARE_TIME` 的 `(t_us, presents)` 端点差（该 `presents`
来自 `Session.svc` 的 vi:obj `binder_presents`，即 SDK IPC present 计数）。每个相位至少需要
两个端点且累计计数必须前进，否则报错。绝对累计 counter 不会被当作 delta，也不会混入
Windows 时钟。

每相位输出 root 样本数、elapsed 的 sum/mean/median/p95/p99/max、编码字节（非 growth
样本的 sum/mean、growth 数）、由端点差计算的 present interval/rate。跨相位按 mode 聚合，
并报告两模式的描述性 delta。**分析器不自动做性能成功判定**；报告字段
`decision` 明确写“仅列描述性 delta”。

## 测量解释

`elapsed_us` 从 enter 处理器取时间开始，到 leave 处理器第一条取时间指令结束。它包含：

- enter 处理器取时间后的目标选择与 frame 写回；
- enter 桥返回时的寄存器/浮点恢复；
- 目标 C 体或原 root 体在 guest 上的执行；
- leave 桥在处理器之前保存 GPR/VFP/FPSCR 的开销。

enter 桥入口的现场保存、enter 处的控制文件轮询、leave 处理器余下工作和 leave 桥的
最终恢复在测量窗口之外。original 与 native 两条路径使用完全相同的 enter/leave 结构和
频率，因此这些固定开销不会被单方面计入某一模式。测量还包含同一进程内其他 guest
线程、host 线程和模拟器调度。

它不包含控制文件轮询（在起始时间之前），但轮询仍进入整帧。它不是纯 CPU 周期，也不是
GPU 时间；不能把 root 时间差直接解释为 FPS 或 GPU 提速。固定场景、相同包和相同相位
窗口仍是必要条件；warmup 只减少边界瞬态，不能消除场景内容差异。

## 验证状态与本轮边界

独立复核宿主 38 项通过（L1），默认 VPK 与采集构建均可编译（L2）。基线 A/B 包
SELF 为 `40a1aca2f5bda08dff271e4dc33a0d0ea94488e82297c9a72ae0e5d03e757448`。
`engine-prepare-ab-baseline-20261104` 在同一进程切换原／基础 C／原／基础 C（L3）。
最初 Windows driver 的原子文件替换参数错误，未按计划切换；没有把计划时刻冒充实际
时刻。修正后继续同一进程，分析以实际 `ENGINE_PREPARE_PHASE` 和完整调用边界为准。
基线第一段 native 较短，不能满足每段 128 个整帧直方图样本门，保留它用于定位准备
开销，不将其作为正式整帧胜负。第二轮使用修正的 driver 和原／优化 C 四段，见 ADR 0022。

`--run` 可复用既有严格整帧工具：要求进程正常退出、恢复核验成功、完整八块直方图、
每段至少 128 个帧间隔、累计差与时间差一致。缺失则失败，不以标题 FPS 或 root 时间
代替。额外输出 p50/p95/p99 的 1 ms 桶范围。控制轮询和 readback 不计入稳定测量窗口。

真机、任务／住宅、自由移动与全部动画仍未验证；同进程 A/B 仍包含调度和场景内容变化。

## 替代方案

- 两次独立进程比较：已被证明无法控制场景和调度差异，不采用。
- 复制整个原函数到 island：可能破坏 PC 相对指令和函数内分支，不采用。
- 在 guest C 中直接读控制文件或调用平台计时器：会把 host TLS/libc 引入 guest 执行
  上下文，不采用；全部 I/O 和计时在 host bridge 处理器内完成。
- 让 leave 也检查 r0：原 root 的任意 r0 会被误判为失败，不采用。
