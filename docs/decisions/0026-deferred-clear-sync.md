# 0026：延迟 clear 的提交与同步契约

- 日期：2026-11-04
- 状态：实现与独立复核通过（L1/L2）；有限村庄 L3 未证明净提速，mode 6 默认关闭
- 范围：原生 GXM 后端；A/B 实验模式，只调整准入 clear 的等待位置，保持原游戏逻辑与绘制内容
- 前置：[ADR 0004](0004-gxm-batching.md)、[ADR 0025](0025-gpu-wait-attribution.md)

## 1. 背景与目标

ADR 0025 的有限村庄复测中，mode 5 的全部通知等待约 21.211 ms/帧，其中
`clear` 约 13.393 ms/帧（约 4 次真实等待/帧）。这些 clear 都是
`depth=0x0` 的全通道颜色 clear：worker 先以 `GXM_BATCH_REASON_CLEAR` 同步提交
开放 scene，`frame_clear_internal` 开头又调用
`gxm_pending_scene_complete("clear")`，随后大多数请求只记录
`s->clear_pending/clear_pending_color`，真正写像素发生在
`frame_clear_in_scene`（入 scene）或 `surface_clear_materialize`（CPU/GPU transfer）。

本轮实现严格单变量的 A/B 新 mode 6 `deep-clear`：**mode 5 的全部行为不变**，
只是把准入的颜色 clear 从“清屏边界同步 drain”改为“先用新的非同步 batch reason
提交开放 scene，仅记录 pending 颜色；在真正物化前证明此前 pending scene 已完成”。
仍走同一个原游戏 root trampoline；deep 纹理池与 idle 回收保持 mode 5 的行为，
不启用 shared/native C/layout。默认关闭，只有 `engine-full-ab.enable` 且控制文件为 `6` 时启用。

## 2. 控制与发布

- `engine-full-mode.txt` 接受 `0`–`6`；`6` 的名称是 `deep-clear`。
- 模式发布（`src/loader/sdk_dso.c`）：
  - 4/5/6 发布深纹理管线开关；
  - 5/6 发布 idle 纹理页回收开关；
  - 只有 6 发布 `vp_engine_clear_defer_enabled`。
- 缺失/非法控制文件仍显式失败（`ENGINE_PREPARE_CONTROL_ERROR`），不回退。
- 模式名、数字校验和三个发布谓词放在 `src/platform/engine_full.h` 的纯
  inline 函数中，loader 与宿主测试共用同一实现。
- `config/patch-plans/engine-full.json`、`docs/reference/markers.md` 与
  `tools/engine/analyze_prepare_ab.py` 已同步；分析器接受 `deep-clear` phase，
  但仍拒绝把控制数字 `6` 当作 phase 名。
- 没有新增 guest 补丁位置：控制文件与三个开关都是宿主状态，
  原字节、补丁范围、ABI 和 ±32 MiB 分支距离检查不变。

## 3. 快路准入（全部条件必须成立）

`clear_defer_admit`（`src/platform/gxm_runtime.c`）是纯谓词，worker 与宿主测试
共用：

1. mode 6 开关为真；
2. GXM worker 已就绪且 context 未 poison；
3. 非 `batch-selftest`，非 pass-dump / capture-input 诊断路径；
4. scene pipeline 处于活动状态（capture 不会把异步路径切成同步）；
5. mask 只含低 6 位，且 depth/stencil 位为 0；
6. 全 RGBA 颜色位（`0x3c`）；
7. surface 已存在，VA 匹配，宽高和格式完全匹配，`color_mem` 有效；
8. 原 `vp_clear_fusion_plan` 确实返回 `defer_colour`（clear fusion 开启、
   不是 dump 边界、可合并旧 pending 值）。

任一条不成立都会走原来的 worker clear 路径：同步 `GXM_BATCH_REASON_CLEAR`
提交开放 scene，再 `gxm_pending_scene_complete("clear")`。资源不存在、尺寸变化、
格式变化、部分通道、组合深度/模板、诊断/捕获和 worker 未就绪都不会进入快路。

## 4. 提交与记录顺序

准入后，`clear_defer_submit_and_record` 先调用：

```c
gxm_batch_flush_worker(GXM_BATCH_REASON_CLEAR_DEFER);
```

该 reason 故意不在 `gxm_batch_reason_needs_sync` 中。它与其他非同步 flush 一样
提交当前开放 scene、记录 pending notification、在切换 arena 前完成 arena-reuse
等待，然后返回。随后才写入：

```c
s->clear_pending = 1u;
s->clear_pending_color = req->color;
s->clear_pending_async = 1u;
```

因此：

- 开放 scene 中已经编码的 draw 一个都不会漏，提交顺序不变；
- clear 记录发生在提交之后，后续 draw/clear 的编码顺序仍与 guest 顺序一致；
- flush 失败时返回原错误，`clear_pending`/颜色/async 标签都不写，clear 请求
  明确失败，不报告成功。

`VPGxmSurface.clear_pending_async` 是“该 pending clear 尚未证明安全”的唯一凭据。
它不是模式开关的镜像：模式切到 5 后仍然保留，直到真正的消费点清除；后续同步路径
合并新颜色时也不会清除既有标签，只有 materialize 成功或 in-scene GPU clear 成功
才清除。

## 5. 同步与失效契约

### 5.1 CPU/GPU transfer 物化

`surface_clear_materialize` 在 async 标签存在时先调用：

```c
gxm_pending_scene_complete("clear-materialize");
```

成功后才清除标签、清除 pending 颜色并调用 `surface_clear`。失败立即返回原错误：
pending 颜色与 async 标签保留，不写任何像素，不释放 GPU 引用，不清除 lease。
这一点覆盖 `draw_materialize_pending_samples` 的“没有开放 scene”情形：该函数只在
`scene_open` 时主动 flush，不能假定一定已经 drain；保护由 surface 标签承担。

### 5.2 入 scene 的 GPU clear

`frame_clear_in_scene` 在该 draw 新建的 scene 内编码 clear。它位于此前所有已提交
scene 之后，依赖同一 GXM context 的提交顺序，因此不需要 CPU 等待。成功后
`surface_clear_consumed_in_scene` 同时清除 `clear_pending` 与
`clear_pending_async`，并更新 `color_clear_mask`、`cleared_count` 和既有计数器。

staging 审查：`frame_clear_in_scene` 的 64 像素色块、顶点和索引按 arena 分开保存
（`gxm_clear_pixels/vertices/indices[arena]`，各 4096 B）。pipeline flush 在切换到
下一个 arena 前执行 `gxm_pending_scene_complete_arena(next, "arena-reuse")`；
非同步 reason 不参与 combined-drain 短路，因此复用 arena 时其上一 scene 已经完成，
不会覆盖 GPU 仍在读取的 staging。

### 5.3 其他消费点

- present、frame-finish、frame-dump：先 `gxm_pending_scene_complete`，再物化；
- surface 销毁/回收：先 drain pending scene 再释放，标签随结构清零；
- backbuffer 的 `surface_clear` CPU transfer 或 GPU blit：只在上述 drain 之后调用；
- 模式 6 → 5：已有 `clear_pending_async` 标签不受开关影响，仍会触发 drain；
- `clear_pending=0` 的所有显式位置只有 materialize 与 in-scene consume，两处都
  同时清理 async 标签。

## 6. 计数、日志与 profiler

- `VP_GXM_CLEAR_DEFER_STATS` 在既有 `static_log_stats` 节奏输出：
  tries / accept / fallback / submit_fail / scene_submits / coalesced /
  materialize_calls / materialize_fail / in_scene / pending_async。
  `tries = accept + fallback + submit_fail`，提交错误原样传播且不记录新 clear。
  主模型复核后将准入路径直接交给提交/记录 helper；单 GXM worker 内不再重复
  查找相同 surface 或重新准入。关闭实验时不执行额外诊断谓词。不新增逐 clear 日志。
- 新 batch reason 名 `clear-defer` 出现在原有 `VP_GXM_BATCH_FLUSH` 与
  `VP_GXM_BATCH_HIST reasons=` 计数中；`batch_reason_hist` 仍为 16 槽。
- `surface_clear_materialize` 的 drain 使用新的固定 profiler 原因
  `clear-materialize`，位于 `gpu_wait_profile.h`，使每行 calls/real/polls/
  failures/block_us 与 `VP_GPU_WAIT_TOTAL` 继续精确对账。原因数由 27 变为 28，
  结构由 2808 B 变为 2912 B。
- 用 `arm-vita-eabi-size` 对照 ADR 0025 的 ELF：text **675644 → 676868 B**，
  BSS **4620752 → 4621168 B**，data 保持 **69192 B**，即 text +1224 B、BSS +416 B。
  surface 结构的对齐不能按单个 unsigned 的 4 B 简单相乘。没有新增 guest 模块补丁。

## 7. 保留的失败与完成条件

以下路径完全保留原实现：

- depth/stencil 校验、拒绝和 bookkeeping；本实验只准入 depth/stencil 位为 0 的请求；
- `sceGxmEndScene`、`sceGxmNotificationWait`、`sceGxmFinish` 的错误传播和 poison；
- pending scene 失败时不退休、不释放 upload pin/direct lease；
- surface resize/release/recycle 前的真实完成等待；
- present/readback/finish/fence 的同步边界；
- clear fusion 的 materialize-before、coalesce 和 dump 规则；
- mode 0–5 的调用顺序、日志原因和等待位置。

未实现或无法证明的情况下不得把 clear 报告为已写入；快路失败会返回原错误，
不存在“成功空 stub”。

## 8. 源码证据（L0）

`src/platform/gxm_runtime.c` 中的 `VPGxmSurface.clear_pending_async` 保存完成义务；
`clear_defer_candidate` / `clear_defer_admit` 做准入，`clear_defer_submit_and_record`
先提交后记录；`frame_clear_internal_impl` 保留未准入的同步路径。
`surface_clear_materialize` 在真正写入前 drain，`surface_clear_consumed_in_scene` 在
同 context 顺序内消费并清理标签。`gxm_worker_process` 的外部与 worker-local 请求
共用此入口。上述纯决策及消费函数通过 marker 抽取供宿主测试，不能悄悄测试复制实现。

## 9. 验证

- 输入身份：`scripts/check-inputs.ps1` 通过（锁定 1.4.0 ExeFS）。
- L1：Windows Release CTest 44 项既有测试全部保留，新增 `clear_defer`、
  `engine_mode` 两项，共 **46/46 通过**。
  - `clear_defer` 由 CMake 从 `gxm_runtime.c` 抽取实际函数体（struct、reason 枚举/
    名称/同步分类、准入、记录、提交、materialize、in-scene 消费），覆盖：
    准入/拒绝、先提交后记录、coalesce 最新颜色、materialize 前真实 drain、
    wait 失败保留标签且不写像素、提交失败保留旧 pending、原同步路径对照、
    in-scene 消费、模式 6→5 后现有标签仍受保护、同步记录不清既有 async 标签、
    新 reason 非同步。
  - `engine_mode` 与 loader 共用 `engine_full.h` 的校验/命名谓词，覆盖 0–6 接受、
    `7`/非数字/EOF 拒绝、名称和三个模式发布谓词。
  - `gpu_wait_profile` 保留原断言并检查 `clear-materialize` 是独立原因。
  - `test_engine_prepare_ab.py` 接受 `deep-clear` phase 并拒绝把数字 `6` 当 phase。
- L2：Vita VPK/SELF 构建成功；产物路径与 SHA256 见下方。
- 主工作区独立复跑输入锁、46/46 宿主测试与 Vita 构建通过；复核简化版 SELF 为
  `e75bbae9990a35c3f59ce2647eeb983d09334de7592287f6fc77c05b3ff1389c`，VPK 为
  `528e1cba52e6c5b289732692e94a33642e4740bbb2b2a74875b16d5264861530`。
- L3 测量结果由下文补充；没有 L4。

构建产物：

- VPK：`C:\Users\15196\Desktop\workspace\MONSTERHUNTER\build\mhgu-clear-wait-review-20261104\vita_port_native_loader.vpk`
  - SHA256 `528e1cba52e6c5b289732692e94a33642e4740bbb2b2a74875b16d5264861530`
- SELF：同目录 `vita_port_native_loader.self`
  - SHA256 `e75bbae9990a35c3f59ce2647eeb983d09334de7592287f6fc77c05b3ff1389c`
- 工具链：VitaSDK arm-vita-eabi-gcc 15.2.0，Ninja + CMake；静态包选择
  `ux0:data/mhgu-vita-port/static-shaders-rg32f`（未放宽身份门禁）。
- VPK 内含 zip 时间戳，最终重打包 VPK 为
  `997500783d749025c629bd9ba1cfdd6811d877b0bd2c53ee0f7a30e57e0d298d`；
  实际运行的是上述 `528e1cba…` 包，两包 SELF 逐字节相同。最终构建通过，未把
  重新打包当成新增运行证据。

## 10. 性能测量与捕获条件

复核发现现有 `b34_dump_due()` 只检查 `b34_dump.enabled` 和延迟，不检查 present/input
捕获是否已经完成。`capture-live inputs=0` 首次捕获后会保持 enabled，原 clear_fusion
会一直把它当作 dump 边界而不再延迟颜色 clear。scene pipeline 虽仍异步，clear fusion
却已经改变。故“只拍 present 不影响所有渲染准备”不能作为测量假设。

本轮性能对照不启用 `capture-live.enable` 或 SurfaceReadback，只保留实际窗口截图。
GPU 读回另跑画面验证，不混入性能统计。本轮不修改旧捕获门控，避免在 5/6 对照中
混入第二个变量；旧带 capture-live 的数值只代表当时的诊断条件。

## 11. 未验证与风险

- mode 5/6 同进程 L3 只覆盖下文的有限村庄窗口，未证明净提速；
- 未覆盖任务/住宅、全部动画、长时间运行和真机 L4；
- clear 延迟改变等待位置而非像素语义；如果后续测量显示 clear 等待转移到
  materialize 或其他边界，应按 ADR 0025 的方法重新归因，不能直接外推收益；
- capture/pass-dump 与 clear 同时启用的组合仍被显式排除，未验证其诊断输出完整性。

## 12. 主工作区同进程对照（L3）

运行 `build/runs/clear-wait-ab-20261104`：Vita3K d7edcbe9、OpenGL、540p、原固定测试
存档与村长对话。SELF `e75bbae9…`、shader index `ed60817f…`、364 variant，与旧稳定
shader 包匹配。`live_capture=false`，不用 SurfaceReadback，只保留窗口截图。
375/435/495/555 秒均为实际游戏窗口、回执非最小化，人物、衣服、地面、建筑、阴影与
经过的羊未见新增明显黑块/缺件。CPU 读回黑图不作为 GPU 验收。

两种模式都使用同一个原游戏 root trampoline；从 mode 5 启动，约 420/480/540 秒
切换为 6/5/6。首段从 360 秒起，各段排除 20 秒，末段只分析 628 秒之前。
分析器的完整直方图、每段至少 128 帧、正常退出与存档恢复门槛未放宽。

| 阶段 | 完整帧间隔 | mean ms/帧 | 通知阻塞 ms/帧 |
|---|---:|---:|---:|
| A1 mode 5 | 256 | 143.159 | 20.213 |
| B1 mode 6 | 256 | 148.433 | 19.323 |
| A2 mode 5 | 224 | 151.789 | 22.034 |
| B2 mode 6 | 416 | 148.743 | 19.239 |

原路径共 **480 帧、147.186 ms/帧**，候选共 **672 帧、148.625 ms/帧**，候选平均耗时
增加 **0.977%**。一对变慢、一对变快；P95 均为 167–168 ms，P99 从 193–194 变为
182–183 ms。不能把尾部改善称为平均帧率提速，不能用跨轮的 157 ms 与本轮横向算收益。

| 通知原因 | mode 5 ms/帧 | mode 6 ms/帧 |
|---|---:|---:|
| clear | 13.976 | 9.269 |
| texture-pool | 6.673 | 7.001 |
| arena-reuse | 0.146 | 2.534 |
| 合计（含其他原因） | 21.063 | 19.271 |

clear 的真实等待从 4.00 降至约 1.86 次/帧，部分等待转移到了 arena-reuse。全部通知
阻塞少约 **1.792 ms/帧**，但纹理解码、查找与 GXM 准备略增，整帧收益未成立。
`clear-materialize` 没有真实阻塞不代表漏同步：其前面常已通过读回/提交边界 drain，
标签驱动的 complete 此时没有 pending；实际调用数另由 clear-defer 计数记录。

四段稳态 `direct_fail`、`direct_quarantined`、实际 staging/upload memcpy、kernel
alloc/free 增量均为 0；资源/绑定/提交拒绝 stage 9–13 增量均为 0。原有 stage 3 拒绝
分别为 0/1/5/20，工作量与自然动画不同，未证明拒绝集合逐 draw 等价。
632.488 秒自然退出，exit 0；存档、markers、原安装 eboot 与模拟器配置恢复核验通过。

当前模块 main `0x8554a000`、SDK `0x8154a000`。原入口 `main+0xbbe9c8` 原字
`0xe92d4ff0`，20 B island `[0x8774a000,0x8774a014)`，分支距离 23336496 B；
SDK 首个 SVC `+0xc0668`、island `0x81f4b250`，已有原字节/范围/±32 MiB 检查均通过。
未新增或放宽补丁位置与身份。

原始证据和输入哈希在该运行目录的 `strict-analysis/report.json`；等待原因分别在
`gpu-wait-phases.json`，budget 在 `budget-phases.json`，资源/拒绝差分在
`counter-phases.json`。独立 GPU 读回另行记录，不纳入性能汇总。

**决定：不采用为默认提速方案。** 保留 mode 6 供研究和回归；不继续围绕“少几次
clear wait”堆微优化。后续应细分 GXM CPU 准备与资源处理，并根据真实依赖安排提交，
而不是直接删除完成条件。真机与全场景/长期运行仍未验证。

### 独立画面验证

`build/runs/clear-wait-visual-20261104` 使用同一 SELF，从 mode 6 启动，显式开启
SurfaceReadback 与 live present capture；不纳入上述性能统计。355 秒捕获前的实际窗口
为完整村庄，随后约 370 秒发起 `inputs=0` 的 GPU present 读回，375/385 秒窗口正常。
960×544 GPU RGBA 文件 SHA256 为
`1d4a32ed4b9fb2d1ec9483c8290e274d5d302f54b292a13a45d90e831c1dd394`，
内容确实包含村庄、角色、地面、建筑、阴影与中文 UI；不是 CPU 黑图或旧捕获。
原始像素文件的 UTC 修改时间位于本进程运行范围内，并与本轮捕获日志的 VA/尺寸匹配；
本地 PNG 仅做无损编码，不改像素。原始文件保存在该运行目录的 `gpu-readback/`。

末次计数 tries=18919、accept=11065、fallback=7854，submit_fail/materialize_fail 均为 0。
注意：捕获请求后 `b34_dump_due` 会阻止新的 fast clear 准入，accept 停止增长；GPU 读回
因此只证明这次捕获及兼容物化路径，不能代替捕获前快路窗口的证据，更不能证明全部动画。
402.673 秒自然退出、exit 0，存档与 markers 恢复回执通过；独立复核安装 eboot 和模拟器
配置均恢复原哈希。真机、任务/住宅、全部 clear 工作集与长期稳定性没有验收。
