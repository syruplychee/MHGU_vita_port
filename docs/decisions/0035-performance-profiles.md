# ADR 0035：正常启动优化组合、A/B 模式 10–12 与单一配置脚本

日期：2026-11-06。继承 ADR 0024（deep 纹理管线）、ADR 0025（空闲页回收）、
ADR 0030（工作日志削减）、ADR 0031（原游戏堆容量）和 ADR 0033/0034（Alpha8
点阵字体）。本 ADR 定义配置契约；最终选择与 L3 复测见[组合报告](../21-performance-consolidation.md)。

## 问题

当前分支正常启动默认启用 deep 64 MiB 纹理策略。历史上单独观察到：

- mode 5（空闲页回收）在旧工作集下有限村庄窗口约 5.25%；
- mode 9（正常日志削减）约 2.0%；
- 单开 mode 7（128 MiB）无效，但 512 MiB 原纹理子堆 + 1664 MiB 总堆后
  取得约 33.7%。

这些数字来自不同轮次和不同场景，不能相加。Alpha8 字体把常用字库 decoded
40 → 11 MiB 后，工作集、miss 率和池压力都已变化，必须把 deep 64、idle recycle、
128 MiB、日志削减、512 子堆、1664 堆和点阵字体作为组合重新测量。完整接管、
native/fast、shared、clear-defer、split 都没有证明整帧提速，不进入正常启动
组合。

同时需要一套可重复、可从 JSON 读取、失败前不修改运行时目录的配置方式，避免
手工摆放 marker 造成实验条件不一致。

## 决定一：正常启动的三个独立 marker

在没有 `engine-full-ab.enable` 时，loader 在补丁阶段启动读取一次：

| Marker | 发布值 | 作用 |
|---|---|---|
| `texture-pool-128.enable` | `vp_engine_texture_memory_mode=1` | deep 统一预算 128 MiB |
| `texture-idle-recycle.enable` | `vp_engine_texture_idle_resize_enabled=1` | GPU drain 前回收空闲异尺寸页 |
| `work-log-reduce.enable` | `vp_engine_work_log_reduce_enabled=1` | 格式化前省略已审计的正常工作记录 |

- 没有 marker 时保持现有默认：64 MiB、无 idle 回收、不削减日志。
- `texture-pipeline.disable` 仍然退出 deep；错误、拒绝、poison/quarantine、pin
  和逐字节 raw 比较全部保留，不因这些 marker 改变。
- 无 deep 时同时出现 128 或 idle marker 会记录
  `ENGINE_TEXTURE_CONFIG_ERROR reason=deep-required` 并返回补丁参数错误，不会
  静默生效、静默回退或忽略 marker。日志削减可以独立于 deep 使用。
- 这些读取和原子发布位于 `engine_full_patch` 的 `!enabled` 提前返回之前，因此
  普通启动（没有 engine-full/native/fast）也会生效。
- `engine-full-ab.enable` 存在时完全不读取上述 marker；AB 模式文件独占控制
  deep/memory/idle/quiet/shared/clear。
- 启动时输出一条
  `ENGINE_TEXTURE_CONFIG source=... deep=.. memory=.. idle=.. quiet=.. shared=.. clear=..`，
  值从原子变量回读，证明实际发布的组合。

## 决定二：A/B 模式 10–12

`engine-full-mode.txt` 原有 0–9 语义不变，新增：

| 模式 | 名称 | deep | 预算 | idle | quiet | clear-defer | split | 执行路径 |
|---:|---|---|---:|---|---|---|---|---|
| 10 | `deep-compact` | 是 | 64 MiB | 是 | 是 | 否 | 否 | 原游戏 root trampoline |
| 11 | `deep-expanded` | 是 | 128 MiB | 是 | 是 | 否 | 否 | 原游戏 root trampoline |
| 12 | `deep-large-quiet` | 是 | 128 MiB | 否 | 是 | 否 | 否 | 原游戏 root trampoline |

10–12 都不选择 native C（1）、fast C（2）或 shared（3），不延迟 clear，
不使用 split raw。原 root、ABI、原字节 guard、输入锁和 GPU 资源生命周期与
mode 4 相同。

控制文件语法：

- 0–9 为单个 ASCII 数字；10–12 为两位十进制，且不得有前导零。
- 允许尾部 ASCII 空白（空格、`\t`、`\n`、`\v`、`\f`、`\r`）。
- 拒绝空白前缀、负号/正号、`:`、`;`、`00`/`01`/…/`09`、`13` 及以上、
  其他非空白尾部和超过 16 字节的 token。
- `vp_engine_ab_control_digit_valid` 仍只表达单个数字 0–9；新增
  `vp_engine_ab_control_parse` 负责完整 token。loader 不再逐字符自行判断，
  宿主测试与 loader 使用同一纯函数。

运行时轮询仍为最多每 500 ms 一次，发布顺序与现有 mode 4 相同：先完整
memory/budget/class，再发布 pipeline 开关；失败显式记录，不回退。

## 决定三：单一配置 JSON 和脚本

`config/performance-profiles.json`（version 1）是唯一 profile 数据源：

- 顶层 `markers` 和 `text_files` 定义全部 managed 文件；每个 profile 必须
  恰好覆盖同一集合，缺项、多项、类型错误、重复、非法 basename 均在写前拒绝。
- 三个 profile：`baseline`、`compact`、`expanded`。`recommended` 选择
  `compact`：新字体往返复测只支持保留日志减负，idle 与 128 关闭。
- `baseline`：原字体（两个 font marker 关）、deep 64 MiB，quiet/idle/128 关，
  512 MiB 原子堆、1664 MiB guest heap。最初 deep-off/1895/850 候选存在大规模
  draw refusal，已排除；不用失败绘制路径宣称性能收益。见组合报告。
- `compact`：deep + `pixel-font.enable`/`pixel-font-native.enable`、512 子堆、
  1664 MiB 堆、日志削减，idle recycle 和 128 均关（对应 AB mode 9）。
- `expanded`：同 compact，另开 128 MiB（对应 AB mode 12）；仅供容量重测，不推荐。
  AB mode 10/11 名称仍表示含 idle 的原测试候选，不等同于最终 profile。
- 三者都：540p、`gpu-present.enable`、`bounded-batch.enable`；不关闭
  `scene-pipeline`/`clear-fusion` 的默认正确路径；`native-movie.enable` 关闭
  （当前无音频，不能为帧率牺牲声音）；engine-full/native/fast/AB/layout、
  game-cost/frame-cost/play-perf/resource-lifecycle/pixel-font-trace 及明显
  诊断 marker 全部关闭。

`scripts/set-performance-profile.ps1 -Profile recommended|baseline|compact|expanded
-RuntimeRoot <数据目录> [-DryRun]`：

1. 先校验完整 JSON、所有 profile、目标根，以及 compact/expanded 所需的
   `font-native/romfs.index`、`romfs.map` 和 `overlay/`。
2. 非 DryRun 时，若检测到活 Vita3K 进程则拒绝写入（忽略 Windows 残留的已退出条目）；写前再次检查。
3. 在第一次运行时写入前，把全部 managed 文件的原始字节和清单备份到
   `local/performance-profile-backups/<时间戳>-<唯一后缀>/`，并写 manifest。
4. 只创建/删除 JSON 列出的 marker、只写 JSON 列出的文本文件；未知文件、存档、
   eboot、字体和日志不删除、不改写。文件删除只用 `Remove-Item -LiteralPath`，
   不使用递归删除。
5. 结果收据写到数据目录 `performance-profile.json`，包含 profile、备份目录和
   逐项变更。
6. `-DryRun` 只验证并输出计划，不创建备份目录、不写收据、不修改任何文件。
7. baseline 是用于有效对照的一组明确值，不是清空目录或恢复原安装；精确回滚读取备份清单。

普通 `scripts/play.ps1` 默认请求 bounded batch；通过 `RunnerOptions` hashtable
传递 runner 参数，保证 switch/数组类型。需要诊断时可以明确关闭 batch。底层
异常和进程/存档保护保留，不从可能残留的 native LASTEXITCODE 推断脚本结果。

## 证据级别与未验证范围

- L0：锁定输入的哈希、原字/guard/ABI/补丁范围均未改动；本 ADR 和 JSON 是
  契约文档。
- L1：61 项 Windows 宿主 CTest 通过。`engine_mode` 覆盖 0–12 位图、10–12
  不启用 native/fast/shared/clear/split，以及完整 token 的正反例；
  `performance_profile` 在临时假运行时树上覆盖 DryRun、Vita3K 活进程拒绝、
  已退出进程条目不阻塞、缺字体/缺 map 提前拒绝、只修改 managed、原始字节备份、基线与 expanded
  恢复、未知文件和 savedata 保留。
- L2：Vita 构建通过。L3：正常/AB 配置实际运行、完整帧间隔和实际 backing
  见[组合报告](../21-performance-consolidation.md)；不把不同历史百分比相加。
- 未验证：真实运行中各 profile 的完整场景行为、内存峰值、任务/住宅/流式
  切换、长时间稳定性、真机 L4/L5、字体包与 512/1664 在所有场景的容量边界。
- 本任务不改原二进制补丁范围、原字体生成器、GXM 内部资源算法、原输入锁、
  ABI 或错误路径。

## 相关文档

- [纹理内存验证](../17-texture-memory-validation.md)
- [资源生命周期与容量](../18-resource-lifecycle-and-capacity.md)
- [原生点阵字体](../20-native-pixel-fonts.md)
- ADR 0024、0025、0029、0030、0031、0033、0034
