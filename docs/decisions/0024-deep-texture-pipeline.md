# 0024：深纹理解码池与 decoder-owned 直接绑定实验

- 日期：2026-11-04
- 状态：当前分支默认启用；旧稳定安装包保留，未自动覆盖
- 证据级别：L0（源码/ABI 静态审计）、L1（宿主回归）、L2（Vita 构建）、L3（Vita3K 有限村庄窗口）
- 真机 L4、全场景与长时间运行未验证；各轮结果见下文，不以复制减少代替整帧结论。

## 背景

[0015](0015-safe-render-performance.md) 与
[渲染性能教训](../13-render-performance-lessons.md) 已记录旧分支直接绑定
decoder-owned 纹理的结果：同窗口 mean 约 141.4 → 112.7 ms，但 pin 住的槽不能换出，
出现 34,686 次 memblock 分配并在约 40 MB 处失败；该路线没有解决持有期和进程级碎片。
同一份教训还证明跳过逐字节纹理比较、内容 hash、按帧间隔校验都会改变内容身份或引入
陈旧数据，不能作为本实验的基础。

本次保留已经存在的有界 Scene 批处理、静态 shader、完整逐字节纹理身份和 GXM
setter 语义，替换解码缓存内存、批处理上传及对应的场景提交/等待过程。

## 决定

### 1. 开关与模式

- `engine-full-mode.txt` 增加 `4`（`deep`）。`0`、`3`、`4` 使用同一个原游戏 root
  trampoline；`1`／`2` 继续是原有 native C 路径。只有 `1`／`2` 检查返回 r0；
  `0`／`3`／`4` 的调用者不消费返回值，任意 r0 合法。
- `3` 继续表示 `shared` 下游共享准备；`4` 不发布该开关，不混入 mode 3 的
  shader/sampler/pair metadata 优化。`4` 只发布一个原子纹理管线开关。
- 普通（非 A/B）运行默认启用该路径。`texture-pipeline.disable` marker 是唯一的独立
  退出开关；A/B 模式下由 `engine-full-mode.txt` 的初始模式与轮询决定，marker 不覆盖 A/B
  选择。draw 路径只做 acquire load，不读控制文件。
- 模式切换只在 guest GPU submission 边界应用：host root 轮询只发布期望原子值；
  实际的 GXM flush/drain、旧 cache 清理和 pool 创建/销毁全部在 GXM worker 上完成。
  切换耗时写入 `VP_TEX_POOL_MODE`，属于 warmup 成本。

### 2. 有界统一纹理池

- 运行路径以总额不超过 64 MiB 的 `USER_RW_UNCACHE` chunk 持有内存，通常每块 4 MiB，
  在块内以 4 KiB 小页分配 raw/decoded region。更大纹理可申请单独大 chunk，仍计入同一
  64 MiB 物理上限。chunk 以 `SCE_GXM_MEMORY_ATTRIB_READ` 映射；raw 只由 CPU 使用，
  从不绑定给 shader。该上限合并原有 48 MiB decoder cache 与 16 MiB upload cache，
  后者不再同时常驻；不是在两套旧缓存上另加 64 MiB。GPU 映射覆盖范围变大，真机仍待验。
- 4 MiB 申请失败时可按资源大小使用较小 chunk（通常至少 256 KiB），成功建块总数最多
  256，物理字节上限仍为 64 MiB。小分配优先进入小 chunk，保留大 chunk 的连续空间。
  本次激活内已失败的大块大小不会每帧重试；能由现有 chunk 服务的请求通过缓存淘汰/
  GPU 完成归还页，不拆掉小 chunk 去反复尝试同一个失败的大块。遇到新出现的更大资源
  才允许重建空 chunk，退出 mode 时重置该分配策略。
- 占用表每页一 bit，按 chunk 的实际页数从 CPU heap 申请；chunk 描述和子分配表的
  最终静态 arena 为 8256 字节。避免为每个 chunk 都静态预留整个 64 MiB 的页表。
- 同 key 的空闲旧版本优先原址更新；其他槽按真正 LRU 淘汰，不为复用小纹理而永久保留
  冷的大纹理。region 的申请/释放只改页表，通常不触发系统分配/映射/解绑；相邻空闲页
  可以直接供不同尺寸使用。只有空 chunk 换尺寸和 mode 退出才解除映射、释放 memblock。
  在拆除不同尺寸 region 之前先回收 GPU 引用，再选择可复用槽。仍被 GPU 读取的块不能改写。
  纯逻辑模块也保留调用者自供 arena 的 first-fit/coalesce 接口，运行路径不申请连续 48 MiB。
- 第一轮申请连续 48 MiB 实际失败（`0x8002710b`），已淘汰该内存方案。那一轮黑帧和
  拒绝绘制产生的高 FPS 完全排除，不能作为提速证据。
- 128 个槽保留原有完整 key：VA、尺寸、TIC format/header/pitch/block height/
  swizzle/type/mip/upload，以及 raw 字节数；命中必须同时通过完整 key 和**全部 raw
  字节**比较。不使用 hash、VA 范围或按帧跳过的验证。`texture-identity-skip-bytes`
  实验 marker 在 deep 路径被明确忽略并记录一次。
- 旧普通 cache（每个槽独立 raw/rgba memblock）保留给 mode 0，身份与解码语义保留。两种
  cache 不同时常驻：切换时先 drain，再释放当前 cache，最后才建立另一种。
- 原缓存也修复“系统分配上限先于逻辑预算到达”的失败：申请失败后先复用同尺寸空闲块，
  或淘汰一个空闲槽再重试；每次重试减少一个占用槽，次数有界。没有空闲槽仍明确失败。
  这解决了切回 mode 0 时 vertex 纹理缺失导致的新增拒绝绘制，不能把漏画当对照加速。
- 新路径不再创建旧 16 MiB upload cache；从 mode 0 切回时，先 drain，再清除旧上传
  关联、解除映射并释放该缓存。mode 0 只在真实复制需求出现时重新创建它，失败明确返回。
- pool 分配失败时，只淘汰 `cpu_pins == 0 && gpu_refs == 0` 的槽。
  若仅剩 GPU 引用槽，报告 `PRESSURE`，由当前 worker 内联 flush 开放 scene 并等待
  真实 notification 完成，然后**只重试一次**。耗尽后返回明确的
  `VP_TEX_POOL_MEMORY_FAIL`；不递归、不死循环、不用新 memblock 逃逸。
- CPU 解码失败或 pool 内存失败对已接受 cache 契约的纹理不再回退到普通 malloc
  解码。失败向 GPU submission 调用者传递，不提前发布 guest fence；旧 malloc 解码只
  保留给本来就不符合 cache 契约的纹理。

### 3. 所有权与更新

- 区分当前 request 的 CPU pin 与 GPU 引用。CPU pin 只保护“解码 → GXM 绑定”的
  当前 worker 临界区；GPU 引用由 GXM 场景持有。
- 新字节出现且旧 RGBA 仍被 GPU 引用时，绝不原址覆盖：分配新版本，或由 pool 淘汰/
  压力路径等待真实完成。任何槽在重写或释放前，先退休 GXM upload/snapshot 关联
  （`vp_gxm_texture_owned_retire`）。
- 同 key 已证明 raw 不同且旧版本只有 GPU 引用时，旧 raw 比较副本不再参与命中，
  可立即归还 CPU 页；旧 RGBA 与 generation 继续有效，最后一个 GPU lease 完成后才归还。
  新版本仍保存并完整比较全部 raw 字节。实测村庄窗口该事件计数为 0，不归因性能收益。
- 每个槽有 generation。GPU lease 只能通过 pool API 从**活槽的 RGBA 指针**解析，
  返回 `(slot, generation)`；旧 generation 的释放被拒绝，不能减到复用槽的新引用。

### 4. 解码

- RGBA8 tiled 解码与 TIC selector 置换融合为一次写出；selector 0、1、2..5、6、7
  的旧映射、GOB 尾部、非 16 倍数宽度和 padding 顺序保持。宿主测试以旧
  `vp_texture_rgba` + `vp_texture_swizzle` 为 oracle 做多 width/height/bh/tail/
  selector 差分，不是镜像实现。
- pitch-linear RGBA8（TIC header 2）同样融合行复制与 selector。
- 其他格式保持原解码。RGBA32F→RG32F 在 identity selector 下增加整行 `memcpy`；
  NaN、-0、subnormal 和任意 32 位模式逐 bit 保留，并有宿主回归。
- 解码 family 时间/字节计数沿用现有 play-perf 门控节奏，不增加逐 draw syscall。

### 5. GXM 上传替换

- 识别 decoder-owned、格式/布局完全符合现有绑定契约的源：batch
  `gxm_batch_reserve_for` 为它预留 **0** 纹理字节；
  `gxm_batch_texture_bind` 直接 retain pool lease 并返回原 RGBA 作为
  `SceGxmTexture` backing。旧 upload cache、snapshot staging 和 memcpy 都不执行。
- primary、extra、cube、vertex 的原受支持格式均可直接绑定。格式映射、布局检查、
  sampler 解析和所有 `sceGxmSetFragmentTexture`／`SetVertexTexture` / sampler setter
  照旧保留；descriptor cache 仍以完整 backing/布局/sampler 键校验。
- 非 decoder-owned、旧池、非 deep 模式或布局契约不符的源走现有精确 copy 路径。
  retain 失败属于内部契约失败，deep 模式显式让该 draw 失败，不伪造 direct 成功。
- 非 batch 路径仍使用原有 copy 路径；直接绑定的覆盖边界就是 batch entry。
- 一帧 inventory 诊断（`m2-frame.enable` 的 auto 录制）会在后续 submission 才重放，
  因此 deep 路径拒绝向它借用 pool 指针（`VP_TEX_POOL_RECORD_BYPASS`，返回
  `UNSUPPORTED`，primary 走原有 malloc fallback）。固定／按时间重放（非 auto）在
  同一 submission 内即时执行，借用指针仍受本 draw 的 CPU pin/GPU lease 保护；事件表
  只对 malloc fallback 拥有释放权，decoder/cache 借用的指针不进入 `free()`。

### 6. 引用生命周期

- `VPGxmBatchEntry` 最多保存 `sampler limit + 1 = 6` 个 direct token（fragment
  tex0..tex4 加 vertex tex0）。draw 成功编码后 token 留在 entry。
- 同步提交在 `sceGxmEndScene` + notification + `sceGxmFinish` 全部成功后释放。
  deferred 提交在 EndScene 成功后把 token 移入对应 arena 的 pending 记录；只有
  notification wait 成功证明 GPU 完成后，`gxm_pending_scene_retire_one` 才释放。
- draw 在发出前失败/回滚：释放该 draw 尚未消费的 token。EndScene 或
  NotificationWait 失败：不释放，移入 quarantine 并 poison，绝不为测试方便而释放。
- 三个 arena 全部覆盖。`gxm_batch_reset_scene_state`、entry admission 的
  `memset` 和 arena 复用前都会先 release/quarantine/move token，不允许静默丢 token。
- pool 压力触发的 flush 在当前 GXM worker 内联执行；从非 worker 路径进入时也只向
  唯一的 GXM worker 提交一次同步回收回调，不等待自身 worker 造成死锁。

### 7. 同步边界

- 不改变 guest fence 语义。`guest_svc` 仍在 submission flush 返回成功后才发布
  `page + syncpoint*16`；没有提前推进 syncpoint 或伪造完成。
- 同一 context 的 scene 按 notification 顺序完成。需要同步的边界改为先把当前 scene
  EndScene 登记，再等待**最新** pending value 一次；该值证明所有更早 scene 完成。
  空 scene 仍在原边界 drain；资源/clear/readback/submit fence 的完成条件不变。
  失败时保留 pending 和 token，poison 并返回原始错误。
- 增加已完成的 notification 轮询：若该 arena 的通知地址当前 acquire 值已经严格等于
  pending value，则跳过重复的 `sceGxmNotificationWait`（已核对本地 Vita3K 实现是
  等值等待）。未相等时仍调用真实 SceGxmNotificationWait；失败路径不改为成功。
  此优化只用于尚未退休、尚未复用的 arena。
- 保留 `sceGxmFinish` 和所有 CPU 可见性/资源释放等待。本轮没有本地证据能证明它们
  可以删除，因此不删除。

接口依据为 [VitaSDK GXM API](https://docs.vitasdk.org/group__SceGxmUser.html) 和本次
运行的 [Vita3K d7edcbe9 实现](https://github.com/Vita3K/Vita3K/blob/d7edcbe9/vita3k/modules/SceGxm/SceGxm.cpp#L2689)。
后者的 `sceGxmNotificationWait` 在 notification word 等于目标时结束；源码证据不能
证明真机缓存维护的全部行为，因此 L4 范围保留。

### 8. 捕获与测量条件

- `gxm_scene_pipeline_active` 不再因为 `b34_dump.enabled` 永久关闭流水线。
  只有实际 surface/pass 输入捕获有效期间才强制同步：显式
  `b34-passdump.enable`（旧行为保留），或 `capture-live` 请求 `inputs != 0` 且
  尚未完成请求的 surface dump 数量。
- 只拍 present（`capture-live` `inputs=0`）保持异步流水线；present 路径本来就在
  `gxm_pending_scene_complete("present")` 之后做 CPU/GPU 读取。
- 输入 capture 完成后恢复流水线。`VP_GXM_PIPELINE_CAPTURE` 记录有效关系；没有把
  draw 中对未完成 RT 的读取引入。

## 计数

不新增逐 draw syscall。深路径沿用既有 256 draw / 512 request 节奏输出累计计数：

- `VP_TEX_POOL_STATS`：alloc/free page map 次数、live/peak 槽、used/peak 字节、
  free runs / largest free run、evictions、pressure waits/failures、
  decode family 时间/字节、retire、direct retain/release/stale/live refs、
  mode transitions 与切换耗时。
- `VP_GXM_TEXTURE_PIPELINE_STATS`：direct binds / bytes avoided、retain/release/
  fail/quarantine、actual upload memcpy 字节与时间、deferred scenes、
  notification poll hits / real waits / notify calls、combined drains、
  pending scene 数。
- direct／combined／poll-hit 计数只在 deep 增加，普通 scene/wait/copy 计数两条路径都有。
  日志只在 deep 曾经生效或当前生效时输出。区间结论必须用
  两个端点的累计值相减，不能把启动累计值当区间计数。
- `staging_memcpy_bytes` 与 `upload_fill_bytes` 在实际 memcpy 点增加，`copy_path_us`
  包含复制路径查询等成本，不能称为纯 memcpy 时间。解码及完整字节比较时间也接回原
  `VP_FRAME_BUDGET`，不能因为换了函数就把时间漏记。计时项有嵌套，不能直接相加。
- `pool_alloc_map/free_map` 在 region 路径表示逻辑 region 申请/释放；实际系统调用看
  `VP_GXM_TEXTURE_REGION_STATS` 的成功建块/释放 `kernel_allocs/kernel_frees`，另有物理字节/峰值和
  suballoc/subfree 计数，不能将小页复用误报成内核分配。页式 arena 的
  `free_runs/largest_free` 对 region 路径为 0，不代表系统没有碎片。系统碎片不作无证据推断。
- 建块失败见 `VP_TEX_POOL_ALLOC_FAIL`；成功建块次数不包含失败尝试，不能只看成功次数
  判断没有重试。`direct_bytes_avoided` 是直接绑定覆盖的 payload 字节上界，不是实际
  少做的 memcpy 字节数；实际复制以 `staging_memcpy_bytes` / `upload_fill_bytes` 为准。
- `VP_TEX_POOL_MEMORY` 只在切换后记录系统接口返回值。Vita3K 的
  [GetFreeMemorySize 实现](https://github.com/Vita3K/Vita3K/blob/d7edcbe9/vita3k/modules/SceSysmem/SceSysmem.cpp#L156)
  按名义上限减累计申请量并夹到零；本轮出现 `user_free=0` 与继续成功建池并存，不能
  把这个字段当成“还能分配几个连续块”的证明。物理缓存上限由本实现的建块字节累计约束。

## 宿主回归（L1）

生产代码按 marker 抽取编译，不复制算法。新增/扩展：

- `texture_pool`：真实页分配器边界、first-fit/coalesce、128 槽、CPU/GPU 引用、
  同尺寸淘汰、generation 陈旧释放、同源内容改写与等值未变化、失败 wait 不释放、
  reset/teardown。
- region 回调模式实测 10,000 次替换保持 4 次 region 分配；同 key 连续 1,000 次更新
  保持 2 次分配；系统提前分配失败时复用合适的已有块；GPU 持有合适尺寸时先报告压力，
  不拆除其他尺寸的块。内容变化且旧版本在 GPU 使用时保留旧像素，另建版本。
- `texture_regions`：生产 chunk 分配器连续 20,000 次不同尺寸替换只申请 1 个 chunk；
  持有区域像素始终不变。覆盖 64 MiB 物理上限、空闲页合并、大 chunk 换入、系统提前
  内存不足、256 个子分配上限和带活数据拒绝 reset。与 `texture_pool` 联合测试证明
  在 48 MiB 限制下三张旧大纹理占位后，新四十张纹理可完整进入工作集，只创建 12 个 chunk。
- 小块受限 provider 测试：先占 12 个 4 MiB chunk，再只允许 1 MiB 申请，仍可填满
  同一 64 MiB 上限；连续 1000 次大块请求既不销毁空闲小块，也不调用 provider 重试；
  大块归还后立即复用。跨 bitmap word 的 65 页分配/释放保持两侧像素不变。
- `gxm_render_pipeline`：production direct token 列表的 6 项上限、释放、未知状态
  quarantine；notification 已完成的 poll 命中不调用 SCE 等待；失败 wait 保留
  pending/direct lease；既有 13 组 completion/hint/blend 检查全部保留。
- `gpu_texture_decode`：融合 RGBA8 与旧解码+swizzle 的多尺寸/GOB 尾部/selector
  差分；pitch-linear 差分；RG32F identity 位模式保留。
- `test_engine_prepare_ab.py`：控制文件接受 `4` 对应的阶段名 `deep`，并继续拒绝把
  数字 `4` 当阶段名。
- 既有 40 项 CTest 未删减、未放宽；分析器的 512 直方图桶与每段 128 帧门槛未改。
  2026-11-04 的 Release 宿主运行共 42 项全部通过（40 项既有测试 + 新增
  `texture_pool` 和 `texture_regions`），命令：
  `scripts/host-tests.ps1 -Python <python> -CMake <cmake>`。

## 失败迭代与测量条件

不能将不同配置的数字横向比较。下表只表示每轮内部的旧/新路径对照；均保留失败证据。

| 迭代 | 旧路径 / 新路径整帧 ms | 处理 |
|---|---|---|
| 连续 48 MiB | 不计 | 分配返回 `0x8002710b`，黑帧及拒绝绘制，整轮排除 |
| 逐纹理 region | 首对 149.825 / 263.760 | 复制省掉但频繁映射/释放，拒绝采用 |
| 加同 key 复用 | 首对约 160 / 231 | 仍有不同尺寸回收抖动，拒绝采用 |
| 先回收 GPU 引用 | 首对约 169 / 153 | 中途启用改善，但切回旧路径新增资源拒绝，不能采用四段汇总 |
| 从启动启用，GPU 读回配置 | 169.281 / 178.789 | 旧缓存回收重试已修正，资源拒绝归零；新路径仍慢，继续改分配器 |
| 48 MiB 小页池，正常配置 | 159.044 / 159.868 | GXM 准备约减半，但解码和等待增加，整帧持平，不能采用提速结论 |
| 只回收已改写旧版本的 raw，GPU 读回配置 | 167.294 / 175.547 | 该事件在村庄计数为 0，不是本窗口瓶颈 |
| 64 MiB、静态大页表 | 不计 | 新增 BSS 改变模块落点，SDK 桥接超出 ARM B 范围，原检查拒绝启动 |
| 64 MiB、只建 4 MiB chunk | 147.573 / 144.079 | 第二次启用只能建到 48 MiB，不能用首段替代整个使用周期 |
| 较小 chunk 回退初版 | 136.390 / 139.450 | 反复拆小块再申请大块，出现 9242 次成功建块，拒绝采用 |
| 最终：保留映射并记住失败尺寸 | 144.966 / 137.858 | 两对窗口都改善，P95 同时降低，采用 |

前五轮本地目录分别为 `build/runs/deep-pipeline-ab-20261104/`、
`deep-regions-ab-20261104/`、`deep-reuse-ab-20261104/`、
`deep-pressure-ab-20261104/`、`deep-final-ab-20261104/`。
第五轮 VPK 为 `fc4239ff3c2c81d7e788f07caaffdda351155d9a182a22978e19a72e2c519d1b`，
SELF 为 `c530a5ac83351a6860a8157d1cffacfde6ad3c13b812a0cd434a2be76ed35ab7`；
它不包含最终小页分配器，不能当最终包使用。

`deep-pages-ab-20261104/` 属于上述 48 MiB 小页池轮次；其 VPK 为
`66690239f0f1ed8c0a2e9dd23b0630dd4e8c79e07b9954c7641ef687845b18b9`，SELF 为
`d2d0cdca456203cfce0276081ffef486beb4dedb5416ae3f5dd8b5d245b276ac`。

第 7–10 轮目录依次为 `deep-version-ab-20261104/`、`deep-budget-ab-20261104/`、
`deep-bitmap-ab-20261104/`、`deep-adaptive-ab-20261104/`，均在 `build/runs/` 下。

最终运行目录为 `build/runs/deep-reuse-pages-ab-20261104/`，普通 OpenGL 配置、540p、固定存档/
镜头/输入，从 deep 启动，按 deep→original→deep→original 四段切换；不启用
`SurfaceReadback` 对模拟器配置的覆盖。每段排除 20 秒，仍要求完整 512 桶直方图、至少
128 帧、正常退出和恢复存档/marker 的哈希回执。原始日志与窗口图保留在本地，不入 Git。

## 最终结果（L3，2026-11-04）

| 顺序与模式 | 纳入完整帧 | mean ms/帧 |
|---|---:|---:|
| deep，从启动启用 | 416 | 135.587 |
| original，切回旧路径 | 384 | 144.625 |
| deep，再次启用 | 416 | 140.128 |
| original，再次切回 | 480 | 145.239 |

合计 deep 832 帧、original 864 帧，mean **137.858 / 144.966 ms**，本窗口平均帧时间
降低 **4.904%**（约 7.254 / 6.898 FPS）。P95 为 **153–154 / 161–162 ms**，P99 均
为 **185–186 ms**；分位数为 1 ms 直方图区间，不能写成小数精度的分位数。

完整 frame-budget 组中，GXM 准备净时间约 **46.637 → 21.714 ms/帧**；与此同时，
解码总时间 **13.525 → 18.768 ms/帧**、notification 阻塞 **10.892 → 21.619 ms/帧**。
所以不能把准备少约 25 ms 写成整帧少约 25 ms，更不能宣称四项总耗时都下降。
这些项有嵌套，与整帧统计的端点也不同，不能直接相加。

- 两个 deep 稳态区间的实际 staging memcpy、upload fill 都为 **0**。保留原始数据完整
  比较和旧 draw/阴影/采样语义；没有靠少画得到结果。
- 首次建 16 块共 64 MiB，第二次建 32 块共 63.75 MiB；两段 warmup 后的成功申请、
  释放和失败申请增量全部为 **0**。第二次 warmup 有一次 4 MiB 失败后改用小块。
  退出后累计建 48 块、释放 48 块，物理字节和所有 GPU 引用归零。
- 稳态 `direct_fail/direct_quarantined/direct_stale` 为 0；原有 stage 3 拒绝两路均存在，
  资源/绑定/提交阶段没有新增拒绝（stage 9–13 区间增量为 0）。
- 315/395/475/555 秒的实际游戏窗口均已人工检查，村长衣服、角色、地面、建筑和阴影
  未见新增黑块或缺件。正常配置的 CPU 读回为黑图，**不把它当 GPU 图像验收**。
  同 SELF 的普通默认启动与显式 GPU 读回另存 `build/runs/deep-release-visual-20261104/`；
  该运行不参与上述性能汇总，也不把读回配置的耗时与正常配置混用。
- 普通启动日志为 `ENGINE_FULL_MODE enabled=0 ab=0`，随后实际应用 deep；没有 A/B
  marker 或 root 接管才能生效的依赖。310 秒 GPU present 读回确实包含完整村庄、角色和
  对话 UI，315/325 秒窗口图也正常。该次运行 exit 0，存档/marker/安装包/配置恢复校验通过。

复核命令：`tools/engine/analyze_prepare_ab.py --sdk-log <run>/sdk-dso.log --gxm-log
<run>/gxm-runtime.log --phases <run>/phases.json --warmup-seconds 20 --run <run>
--output <fresh-dir>`。完整报告在 `<run>/strict-analysis/report.json`。

## 证据与未验证

- L0：没有新增游戏补丁位置，原字节、范围、ARM 跳转距离检查全部保留。静态内存影响
  模块落点：较大静态页表曾使 SDK 落到 `0x87807000`，在第一个 SVC 处返回
  `VP_PATCH_REACH=-6`，没有放宽检查或把失败改成成功。最终页表紧缩后 SDK 为
  `0x81549000`；首个 SVC 的原字节 `0xef000001`，范围 `[SDK+0xc0668, SDK+0xc066c)`，
  76 字节 island 在 `0x81f4a250`，其中 +64 的桥接 B 为 `0xeac3141f`，距离
  `-15970180` 字节，处于原来的 ±32 MiB 检查内。其余位置逐项由既有 ledger 验证。
- L1：宿主 CTest 42/42 通过（2026-11-04，Release）。真实 SDK 捕获对照继续覆盖
  7881 次调用、3033 次真实查询、158000 字节编码和 571 项资源描述；它证明上游编码
  契约未改，不能代替新 GXM 内存路径的运行验收。
- L2：Vita VPK/SELF 构建通过（2026-11-04，`mhgu-texture-regions-20261104`）；
  最终封包 VPK SHA256 `abe277969bd8dfbebfbd370fd15a6bbb4052b7a4d2d75a68df6cc66fedcbc755`，
  SELF SHA256 `4c032d02e2ca8558d543faa5037ec656404e8e8b602d3316ba52057d27412b25`。
  A/B VPK 为 `4e1b9a1638b035b3f8747a31fd64bc23380b3766258a5ecde3dfc5227d838e97`，
  两包 SELF 逐字节相同。最终重新封包的 VPK 另做普通默认启动与图像验收。
- L3：有限村庄画面、从启动启用和双向切换；性能范围见最终运行数据。没有 L4。
- 稳定 VPK `fd7432b75cb38be39483218893fd31972b5fa09b768d31eadf9625c8c7398463`、
  已安装 eboot `d9d28e97846de490d50b862ccc8d91b3a3c021b846cf6330d21db93ef8e9c2ed`、
  runtime config `5d1d55e0177796722e4dacedd72c991ac349ec7fa7849e3705dc1a8f8f5829be`
  均保持原哈希。交付新包另放 `build/candidates/mhgu-render-pipeline-20261104.vpk`。
- 未验证：真机 `USER_RW_UNCACHE` + GPU READ 映射的真实
  一致性、GXM 直接采样该映射的行为、长时间资源流式换入换出、所有动画/场景、
  真机 cache 与命令处理器语义。

## 替代方案

- 继续每纹理 memblock + 全局 pin：旧分支已出现 34,686 次分配和 40 MB 失败，拒绝。
- 内容 hash / 跳过 raw 比较：不能证明字节相同，违反现有身份契约，拒绝。
- 每帧一次或间隔 N 帧校验：会延迟写入可见性，拒绝。
- 猜测 Dcache 维护 API：本地接口不足以证明写回语义，不采用。
- 扩大 batch limit / uniform ring：与内存所有权问题无关，不采用。
