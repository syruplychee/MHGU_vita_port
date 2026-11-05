# 0029：纹理内存容量与 raw/decoded 分离实验

- 日期：2026-11-05
- 状态：实验实现完成，默认关闭；L0（源码/ABI 静态审计）、L1（宿主回归）已验证
- L2（Vita 构建）、L3（Vita3K 同进程 A/B）与真机 L4 待主 agent 独立执行
- 本文不包含性能结论，也不认为 128 MiB 适合真机

## 背景

[0024](0024-deep-texture-pipeline.md) 的 deep 路径以 128 个槽、合计 64 MiB 的
`USER_RW_UNCACHE` region 保存完整 raw 快照与 decoded RGBA：raw 仅供 CPU 逐字节比较，
decoded 才是 GPU backing。`gxm_texture_chunk_create` 把每个 chunk 映射为 GXM READ；
region provider 当前忽略 raw/decoded 的差别。因此两个问题可以分开验证：

1. 64 MiB 是否确实是重复 miss/等待的容量约束。若是，合计预算扩到 128 MiB 后，同一
   工作集应能同时驻留超过 64 MiB，并且实际申请量仍不超过 128 MiB。
2. CPU-only raw 是否不需要 uncached + GPU map。若把 raw 改为普通 cached、不映射的
   main memory，decoded 仍是 uncached + GXM READ mapping，可单独观察 CPU 比较/保存
   路径与 GPU 可见性是否受影响。

这两项是**独立实验**，不能同时启用。可在同一进程分别与基线交替，以排除跨构建差异。

## 开关

沿用 `engine-full-ab.enable` 与 `engine-full-mode.txt`。控制文件仍只接受一个 ASCII
数字加尾部空白，合法范围扩为 `0`–`8`；缺失或非法仍显式失败，不回退到候选路径。

| mode | 名称 | deep | 纹理内存配置 | idle resize | clear defer |
|---:|---|---|---|---|---|
| 4 | `deep` | 是 | 0：64 MiB，raw/decoded 同 class 0，uncached + mapped | 否 | 否 |
| 7 | `deep-large` | 是 | 1：128 MiB，其余同 mode 4 | 否 | 否 |
| 8 | `deep-split` | 是 | 2：64 MiB，raw class 1 cached 且不映射；decoded class 0 uncached + mapped | 否 | 否 |

旧 mode 0–6 的语义不变；7/8 都使用原游戏 root trampoline，不启用 mode 5 的 idle
resize，也不启用 mode 6 的 deferred clear。普通启动不读该控制文件，仍走 mode 4 的
64 MiB、统一 class 0 配置。

## 原子发布与切换边界

- 新增 `vp_engine_texture_memory_mode`：0 = 64 MiB 统一，1 = 128 MiB 统一，2 = 64 MiB
  分离。budget 与 class 由同一个原子值决定，避免“先换 budget、后换 class”的半更新。
- A/B 轮询先发布该原子值，再发布 deep 开关；关闭时先清 deep 开关。4/7/8 之间
  deep 始终开启，budget 与 class 由一次原子读共同确定；其他旧模式的独立开关不变。
- `texture_pipeline_apply_pending` 不再只比较 deep bool：mode 4 ↔ 7/8 即使 deep 仍为
  1，也必须走完整切换。切换在既有 GXM worker submission 边界完成：
  `vp_gxm_texture_pipeline_reclaim()` flush/drain 所有 scene → 检查 GPU refs 必须为
  0 → reset pool 释放全部 region → reset region arena → 按新 budget/class 重新
  configure 和 init。GPU 引用未归零、reset 失败或配置非法均返回错误；不发布成功、
  不继续按新配置绘制。拆除开始后失败不承诺回滚到原池，由既有提交失败路径处理。
- 切换日志新增 `VP_TEX_MEMORY_CONFIG`、扩展 `VP_TEX_POOL_MODE` 和
  `VP_TEX_POOL_MEMORY`，明确 `memory_mode/budget/split/raw_class`，并记录切换前后
  实际 raw cached、decoded mapped、总 physical/peak 和 user/CDRAM free。模式切换本身
  是 warmup，不用于正式测量。

## 单一共享预算与 allocation class

`texture_regions.h` 仍是唯一的纯逻辑 region 分配器（运行路径只有这一个
`VPTextureRegions`），新增：

- `class 0`：decoded / unified，uncached，GXM READ mapped；
- `class 1`：split raw，cached，CPU-only，不调用 `sceGxmMapMemory`；
- 动态 budget：默认调用明确传 `VP_TEXTURE_REGION_BUDGET`（64 MiB），实验最大
  `VP_TEXTURE_REGION_MAX_BUDGET`（128 MiB）。

约束：

- 每个 chunk 只属于一个 class；请求只在同类 chunk 内 first-fit/coalesce。raw 与
  decoded 在 split 模式永远不会来自同一个 chunk；unified 模式仍允许共用。
- `physical_bytes` 与 `budget` 各只有一个，class 只做分项统计；不存在“两个各 64 MiB
  的 pool”冒充 64 MiB 总预算。
- 空 chunk 可以释放并按新 class 重建；含任一 live 子分配的 chunk 不可回收。相邻空闲
  页仍可跨尺寸复用。
- 失败 block 大小按 class 分别记录，class 1 的失败不会阻止 class 0 的正常重试；也
  不会每帧重新申请同一个已知失败尺寸。
- texture_pool 的 128 槽、CPU pin、GPU lease、generation、逐字节比较、pressure 单次
  重试、retire 顺序均未改变。begin_store 的 raw 请求 `raw_class`，decoded 固定 class 0。
- 累计 alloc/free 计数跨模式保留；`peak_bytes` 在重新 configure 时从 0 开始，旧模式的
  峰值已由切换前的 `VP_TEX_MEMORY_CONFIG` 记录，避免 7→8 后把 128 MiB 峰值误当成
  mode 8 的常驻量。

## 运行路径映射

`gxm_texture_chunk_create` 接收 allocation class：

- class 0：`SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE` + `sceGxmMapMemory(...READ)`；
- class 1：`SCE_KERNEL_MEMBLOCK_TYPE_USER_RW`（cached main memory），不映射、不绑定
  shader。

`VPGxmTextureRegion` 记录实际 `mapped` 标志；释放时只有 mapped chunk 才遍历 texture
unit 清除绑定并 `sceGxmUnmapMemory`，cached raw 只做 `sceKernelFreeMemBlock`。raw 只
是 CPU 快照，不做任何 GPU cache flush；decoded 仍是 uncached，不允许改成 cached 后
遗漏可见性维护。

## 计数与验收线索

保持既有 `VP_GXM_TEXTURE_REGIONS` 字段兼容，并增加 `budget/memory_mode` 与逐 class
的 chunk/allocated 字节。每 32 个 present 在 frame-cost 同一端点输出
`VP_TEX_MEMORY_METRICS`（独立于 frame-cost 是否开启，仅在 deep 已应用时）：

- `raw_cached_bytes` / `raw_cached_alloc_bytes`：class 1 的 chunk 与真实子分配字节；
- `decoded_mapped_bytes` / `decoded_mapped_alloc_bytes`：class 0 的 chunk 与真实子分配；
  unified 模式下这项同时包括 raw，不能当作纯 decoded 占用；split 模式才只有 decoded。
- `physical_bytes` / `peak_bytes`：唯一的合计实际申请量与峰值；
- `pool_used/pool_peak/live/peak_live/evictions/pressure/nomem/gpu_refs`；
- `user_free` / `cdram_free`。

这些值用于判定：mode 7 是否真的驻留超过 64 MiB 且总 physical ≤ 128 MiB；mode 8 的
raw chunk 是否 cached/unmapped，decoded 是否仍 mapped；总 physical 是否 ≤ 64 MiB。
预算字段只表示配置，不得当作占用。

注意：历史运行出现过 `user_free=0` 仍能继续建块的情况；不把该模拟器字段当作真机
内存余量。容量结论以本实现的 `physical_bytes`/实际 `kernel_allocs` 为准。

## 静态内存影响

`VP_TEXTURE_POOL_MAX_PAGES` 随最大预算从 16384 页增到 32768 页，`VPTexturePool` 的
静态 `page_map` BSS 增加 16384 字节。本次本地增量构建的符号大小（仅用于报告静态
增量，不作为 L2 验收）：

| 符号 | 大小（字节） | 变化来源 |
|---|---:|---|
| `deep_texture_pool` | 46232（`0xb498`） | 页表 +16384，`raw_class` 至多 +4 |
| `gxm_texture_region_arena` | 10328（`0x2858`） | allocation/chunk class 与分项计数，ARM32 约 +2072 |
| `gxm_texture_regions` | 3072（`0xc00`） | 每块 mapped 元数据 +1024 |

合计约 **+19 KiB** BSS；逻辑/物理预算默认仍是 64 MiB，128 MiB 只在 mode 7 显式
切换后配置。宿主 x64 实测 `sizeof(VPTexturePool)` 从 33976 增至 50368（+16392），
只作结构变化参考，目标以 ARM32 构建为准。Vita 构建必须重新检查 SDK DSO 落点、首个
SVC 的 branch reach 和既有 patch ledger；本轮增量未在运行期验证，若超限必须明确
失败，不得放宽检查。

## 宿主回归（L1）

保持并扩展既有断言，不删除、不弱化：

- `texture_regions`：128 MiB 预算可实际容纳 >64 MiB 且不超过 128 MiB；split 两类
  共用一个 64 MiB 预算且不混用；空 chunk 换 class；有 live 页的 chunk 不被回收；
  相邻页复用；失败尺寸按 class 隔离；默认 64 MiB 行为保留。
- `texture_pool`：split raw 走 class 1、decoded 走 class 0；两类 base 不落在同一
  chunk；64 MiB 上限；第九组同尺寸内容触发淘汰但不增加 physical；小尺寸混合请求
  复用两个 class 的空 chunk。
- `engine_mode`：控制字符合法边界 0–8、mode 名称、deep/idle/clear 谓词，以及
  4/5/6→0、7→1、8→2 的内存配置映射；7/8 不启用 idle/clear。
- `test_engine_prepare_ab.py`：接受 `deep-large`、`deep-split` phase 名，旧名称和
  数字 phase 名拒绝逻辑不变。

## 未验证

- L2：Vita SDK 构建、SELF/VPK、SDK DSO 落点与 ARM branch reach。
- L3：Vita3K 同进程 A/B 的 mode 4 对 7、mode 4 对 8；真实驻留、miss/等待、整帧和
  图像对照，以及模式切换 warmup 排除。
- L4：真机 cached `USER_RW` raw 的 CPU 一致性、uncached decoded 的 GPU 可见性、
  128 MiB 的进程内存压力、长时间流式资源与所有场景。
- 本文不声明任何性能收益，也不建议把 128 MiB 作为真机默认值。
