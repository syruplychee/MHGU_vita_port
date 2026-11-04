# 0023：下游共享 shader／纹理资源与 GXM 准备实验

日期：2026-11-04。状态：实验，默认关闭；实现、宿主回归、构建及 Vita3K 同进程对照
已完成（L1／L2／L3）。观察到小幅改善，尚不认定稳定净提速；真机未验证。

## 背景

[ADR 0022](0022-engine-prepare-shared-queries.md) 的同一稳定阶段帧预算显示，整帧约
184.5 ms 中公共 root 不到 1 ms，而命令分派内部包含约 163 ms，其中 `draw_prepare`
包含约 139 ms（含同步等待，字段有包含关系，不能相加）。继续只优化公共 root 的小循环
收益上限很低。下一处有依据的替换在公共材质／资源数据进入下游绘制准备之后：

- `guest_svc` 每个纹理槽重复调用 shader 采样器声明、静态资源状态和资源 sidecar 查询；
- 已经创建并验证过 blend 输入的 `VPStaticPair` 在后续 draw 又重建 GXM blend，
  批次 uniform 预算又重复读取 resident 默认 uniform 大小；
- primary／extra／vertex sampler 每次都重新解析同一组 TSC 枚举，批次预检查还会再解析一次。

这些工作不改变游戏数据、绘制顺序、纹理内容或 GXM 绑定，只重复准备可证明不变的结果。

## 决定

沿用原游戏 root、队列、材质、资源、NVN 编码和 GXM 后端。新增一条默认关闭的
mode 3（`shared`）路径，复用已有 `engine-full-ab.enable` 同进程 A/B 框架：

- `engine-full-mode.txt` 新增 `3`；`0` 与 `3` 都执行同一个原游戏 root trampoline，
  只有 `3` 发布下游 shared 准备开关；`1`／`2` 维持原有 C 路径并关闭 shared。
- 开关在 host TLS 的原 root 轮询处、root 计时开始前用原子 store 发布；draw 侧只做
  acquire load，不读控制文件。模式切换本身不创建、销毁或改写任何 GPU 资源。
- 只有 native C 入口（`1`／`2`）检查返回 r0；`0`／`3` 走原 root，其调用者不消费
  返回值，任意 r0 合法。
- 旧的逐槽 API、旧 blend／resident 查询和旧 sampler 转换保留为 oracle 与 legacy
  路径。mode 0、legacy shader 包或任何计划不可用时仍执行旧路径，不伪造成功。

### 1. 批量 shader 资源计划

`vp_gxm_shader_resource_plan(token, gpu_stage, viewport, out)` 为一个 stage 返回
tex0..tex4 的 `declared_mask`、当前 `resources_active`、每个已声明槽的原始
`vp_gxm_shader_resource` 返回码和 `VPResourceTexture` 值拷贝。计划不包含 resident
指针。构造（miss）时逐槽调用原 API，因此返回码、未声明、缺布局、缺 resident 和
失败语义与旧路径相同；命中时只复制值，不再执行逐槽查询。

`guest_svc` 在 mode 3 下每 draw 至多查询 VS／FS 各一次计划，primary、extra 和 vertex
槽直接取结果。若计划 API 返回 `-3`（legacy 包）或参数错误，整段回退旧逐槽路径并输出
有界 `VP_GXM_SHARED_PLAN_FALLBACK`，不会静默关闭纹理解析。

缓存有界（64 项），键为严格 `(token, gpu_stage, viewport)`，并校验
`static_plan_generation`。resident 注册、resident 卸载／驱逐和索引重新加载都会增加
generation。尤其“先前没有 resident，计划缓存了未声明结果，之后才注册 resident”的
场景，下一次查询必然重建，不能保留旧 false。

### 2. GXM pair 准备结果

`VPStaticPair` 创建时已经完整比较 blend 输入并成功构造 `SceGxmBlendInfo`。现在同时
保存该不可变结果、VS／FS 默认 uniform 大小和 fragment 是否声明 `vp_face_state`。
mode 3 的 pair 命中不再重复 `build_blend_info`，批次 uniform 预算直接用保存的大小；
每 draw 的 `cull_enable`／`cull_face` 仍实时决定是否追加第二份 FS 默认 uniform，
继续使用原有的饱和加法及上限检查。pair 释放／重建通过 `memset`／重新赋值自然失效，
不使用地址猜身份。

### 3. 共享 sampler 计划

纯逻辑 `gxm_sampler_plan.h` 解析当前 `sampler[2]` 的 min／mag／mip／U／V 到 GXM
枚举，并使用 64 项全键缓存（kind、两个原始 word）。primary 只应用 min／mag／U／V，
刻意不设置 mip filter；extra 与 vertex 应用 mip，vertex 的 RG32F 例外照旧覆盖。
非法枚举仍由实际 apply 位置输出与原来相同的日志并返回 `-2`。批次预检查与真实 apply
共用同一缓存计划，避免重复转换；所有 `sceGxmSetFragmentTexture`／
`SetVertexTexture` 调用保留，不缓存像素内容，也不以地址代替内容检查。

## 同步与生命周期

GXM worker 和 guest 解码入口由 `gpu_execution_gate` 串行化：worker-backed 解码入口在
整个回调期间持锁，直接路径也在同一把锁下执行。shader 计划查询、resident 注册／驱逐、
pair 创建、sampler 预检查与 apply 都落在这一个解码临界区内；计划缓存与 resident 表的
generation 检查因此是失效判断，不是新增锁假设。现有代码中 resident 表和 pair 表也只
在 GXM worker／同一临界区内修改。

GXM sampler 计划缓存只在 `frame_draw_internal`（单 GXM worker）使用，不需要额外锁。
计划中只保存枚举与值结构，不延长任何 GPU 资源生命周期。

模式开关是进程级原子值，只在 host root 轮询处发布；同一 root 期间的其他 guest 绘制
线程可能在下一次 draw 才观察到切换。两条路径对同一输入产生相同的值，切换本身不改
GPU 资源，因此这只影响相位边界的严格程度，由测量 warmup 排除，不是画面正确性条件。

## 计数与日志

在现有 `VP_GXM_STATIC_STATS` 节奏（每 256 draw）额外输出
`VP_GXM_SHARED_PREP_STATS`：shader plan hits／misses／builds／invalidations、VS／FS
调用数及 tex0..tex4 声明覆盖、sampler plan hits／misses、pair prepared hits、
uniform plan reuse、blend plan reuse 和当前 generation。计数为累计值，实验后应使用
阶段端点差值，不把启动累计值当区间计数；不新增逐 draw 日志或计时 syscall。

## 未替换与未验证

- 未替换：原游戏 root／队列生产、资源创建、TIC／TSC 当前字节读取、纹理解码与上传、
  深度 view／GPU 生产者／阴影检查、pair miss 的创建、resident 注册、NVN 编码、GXM
  命令解析、所有 draw／clear／present 和纹理 setter。
- legacy shader 包下 shader 计划主动返回不可用并走旧路径；sampler 计划与静态包无关，
  但仍只在 mode 3 启用。
- 宿主新增 `gxm_shader_plan`／`gxm_sampler_plan` 与 `gxm_render_pipeline` 的 blend／
  uniform 边界回归；全部 40 项 CTest 通过（L1）。shader 计划测试使用受控 query 回调，
  不把它表述为真实 GXM 或原二进制全场景差分。带改动 VPK／SELF 构建通过（L2）。
- L3 仅限下述村庄固定机位、自然 NPC／动画采样；未覆盖全部动画帧、场景切换、任务、
  长时间资源流式换入换出及真机（L4／L5）。

## 2026-11-04 实测与判断

主工作区独立复跑输入身份检查、40 项宿主测试、Vita 构建。实际测试 VPK SHA256：
`2a40f60e775414f858107ecd22675d4ce29cd0e99b7624ef7c5ad69cfd681ea8`；SELF：
`a0c0ea6c14d833099f88a5e9dc8b03c8aa8abfa4a9205c1d5279847719407efb`。
稳定 shader 包及 Vita3K OpenGL 与 ADR 0022 相同。运行时强制 surface readback，
`disable-surface-sync=false`，结束后恢复原配置。

对新 ELF 复跑真实 SDK 编码回归（旧固定队列采集）：7881 次调用、3033 次查询、
158000 字节命令及 571 项资源描述一致；命令 SHA256 仍为 `89c60ed5…`。
这只证明原提交编码路径没有被本轮改动破坏，不证明新 shader／GXM 准备的全部运行行为。

### 有效同进程对照

运行目录（ignored）：`build/runs/shared-prepare-ab-repeat-20261104`。502.188 秒正常退出，
exit 0，`caller_stop=false`。存档、markers、已安装 eboot、运行配置恢复并核对哈希。
无并行旧游戏进程；稳定 `build/vita` VPK 与原安装 eboot 未替换为实验版。

控制实际切换时间（游戏微秒时钟）：270426203 → shared，340632809 → original，
410247092 → shared。两个模式使用同一个原 root trampoline。首张 215 秒截图仍在加载，
因此首段分析起点从 219 秒开始，排除 20 秒后，再由完整帧直方图端点选取有效范围；
不把加载页的高 FPS 混入村庄。后续各段同样排除 20 秒。

| 阶段 | 实际整帧统计区间（秒） | 有效帧间隔 | 平均 ms/帧 |
|---|---:|---:|---:|
| 原路径 A1 | 239.570–264.426 | 128 | 194.191 |
| shared B1 | 294.793–337.610 | 224 | 191.146 |
| 原路径 A2 | 362.095–405.055 | 224 | 191.789 |
| shared B2 | 435.122–496.373 | 320 | 191.410 |

严格整帧门禁检查全部 512 桶、区间累计差、帧时间和时间戳一致，以及每段至少 128 帧。
分析文件：`build/engine-full/shared-prepare-repeat-metrics/report.json`；phase 输入：
`build/engine-full/shared-prepare-repeat-phases.json`。四段范围分别为
`[219000000,270200000]`、`[270500000,340400000]`、
`[340700000,410000000]`、`[410300000,498000000]`，warmup 20 秒。

合并原路径 352 帧 **192.663 ms / 5.190 FPS**，shared 544 帧
**191.301 ms / 5.227 FPS**，平均少 1.361 ms，约 **0.71%**。
原路径／shared 的 p50 为 189–190／188–189 ms，p95 为 222–223／218–219 ms，
p99 为 249–250／250–251 ms。两组相邻均值同向，但幅度小，p99 没有改善；
A1 只有最低门槛的 128 帧且紧邻进村阶段。**不认定统计上稳定提速，保持默认关闭。**

280、350、420 秒 GPU 实际回读对应 shared／original／shared，检查了村长衣袖、玩家、
背景建筑、地面、阴影，最后一张还有羊；这些采样没有发现新增黑点黑块或大面积漏绘。
动画／NPC 自然移动，不能作逐像素差分或全部动画帧无错的证明。A1 没有进村后的独立
GPU 采样，对其场景判断还依赖队列与后续固定机位连续性，这进一步限制性能结论。

### 重复准备确实被替换

统计用上述预热后区间内的累计计数差，非启动总数。原路径两段的新计数差全部为零。
shared 两段合计：

- shader plan 命中 417526、miss／build 122862，命中率约 77.3%；VS／FS 各 270194 次。
- VS tex0 使用 109195 次；FS tex0／1／2／3 分别 148969／60919／40484／10936 次，
  tex4 未观测。它覆盖角色、场景与多纹理绘制的共享入口，不限某个 shader。
- sampler plan 命中 273668、miss 19931，命中率约 93.2%。
- pair 准备、uniform 计划、blend 结果各复用 269568 次。
- 稳定窗口 generation 未改变，stale invalidation 差为零；资源注册／卸载的失效正确性
  仍主要由源码和受控宿主回归支持，不能称作流式场景 L3 通过。

统计与整帧端点不同，不能把上述次数直接除以整帧表中的帧数。
细节保存在 `build/engine-full/shared-prepare-repeat-counters.json`。
GXM 拒绝仍存在：整次运行累计 100（stage3=27、stage9=73）；四段预热后差分别
7／4／4／8，区间长度和自然绘制工作不同。未证明逐 draw 拒绝集合完全一致，不能声称
全部绘制已通过，也没有绕过这些拒绝来压低帧时。

独立的整帧预算窗口（384 个原路径／576 个 shared 帧）中，`gxm_prep_net` 为
48.763／47.967 ms/帧，`draw_prepare_incl` 为 141.700／140.624 ms/帧；
纹理内容 lookup 为 12.796／12.615 ms，decode 为 16.503／16.444 ms。
这些包含字段不能相加；本轮保存准备结果没有替换解码、内容比较、资源创建、提交和同步。
剩余工作应继续从这些实际成本与场景／资源生命周期取证，不因本次小收益否定上层接管。

### 首轮没有用于正式性能结论

`build/runs/shared-prepare-ab-20261104` 中控制脚本因 Windows 文件替换错误提前结束，
随后在同一进程恢复切换；实际时刻为 322978893／379608815／439420200，第一次 GPU
回读也推迟。保存了画面／计数，正常退出且完整恢复，但该轮条件不齐，不纳入上述均值。
第二轮使用原子替换及短暂共享冲突重试的输入驱动，控制日志无错；没有修改游戏来让测试通过。

## 替代方案

- 给旧逐槽 API 内部加缓存：会改变 oracle 和 legacy 语义，无法做严格差分，不采用。
- 在 guest_svc 缓存完整纹理描述或像素：地址／内容失效条件无法证明，不采用。
- 命中时跳过 GXM setter：已有 Vita3K dirty-state 教训，不采用。
- 让 mode 3 也走 native C 入口：会混入 root 接管变量，不能做单变量对照，不采用。
