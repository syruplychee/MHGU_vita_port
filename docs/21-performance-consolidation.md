# 性能分支成果与最终组合复测

日期：2026-11-06。整理分支 `codex/performance-final`，继承 `9b01426`。

## 本轮要回答的问题

历史实验的收益不能直接相加。128 MiB 大池曾经解决旧 RGBA 字体反复解码，
但现在字体已改为 Alpha8，实际显示缓冲区也改正为 720p 预设，必须重新选组合。
本轮先固定新字体和画面，对日志、空闲页回收及池容量做同进程往返；再关闭 AB
和细分探针，比较正常启动的完整组合。

**推荐 `compact`：deep 64 MiB、日志减负、Alpha8 原生字模及 1:1 显示，
关闭 idle 回收和 128 MiB。有效正常启动对照合并为 139.246→81.051 ms/帧，
7.182→12.338 FPS；帧时间 -41.79%，FPS +71.80%（1.718 倍）。P95 162–163→95–96 ms。
这是有限村庄 L3，尚未达到 30 FPS，也未验证真机。**

## 测量条件与比较边界

- 锁定完整 1.4.0，继续保留原 AArch32 游戏逻辑；输入检查通过。
- Vita3K `d7edcbe9`，OpenGL；模拟器 SHA256
  `dd68489a2ddb1cc7910712c2eb5925a0b2af70c507f6703a28d3a43815479d13`。
- 同一候选 VPK SHA256
  `7cec4fc5abfcafaa068658b247bb8992bb424f7ac63bbc58389ee2487dc1de70`；
  SELF/运行 eboot SHA256
  `fd1e39a94ef4fe95b66ec1de7094f939244d70c2cfb277891aa3e6add1a35ac1`。
- 相同 RG32F 364 variants 包、540p 配置、有界批处理、GPU present，窗口客户区
  960×544；同一受保护测试存档，固定村长对话页，200 秒后无按键输入。
- `play-perf`、`frame-cost`、`game-cost`、资源 census、字体 trace、GPU live capture
  全部关闭。只在正式计时窗口之外拍 Windows 窗口图。
- 正常 SDL 音频开启，未设置 dummy 音频驱动；原生视频-only 播片关闭。
- 取实际显示帧之间的间隔，累计端点相减；512 个 1ms 桶计算 P50/P95。
  每窗口不少于 128 帧，完整直方图、时间戳/区间和、正常退出和存档恢复必须通过。
  不用启动均值、窗口标题 FPS 或 Present 函数自身耗时冒充整帧。
- 有效基线是**上一阶段仍能正确提交主要绘制的 deep 64 MiB + 原 RGBA 字体**，
  日志减负关闭；512 MiB 纹理子堆、1664 MiB guest heap 与最终组合相同。RG32F、
  快速锁、Session 门锁、Scene 流水线等已有公共路径两侧一致。因此结果衡量原字体
  到最终字体/显示/日志组合的净收益，不能称为性能分支最初版本到当前的完整加速比。
- 最终组合同时改变字体、日志策略和原最大显示缓冲区预设；新模式画面更清楚，
  不是同一批中间像素。总收益可以用于选择整套配置，不能全部归因给 Alpha8。

## 一、同进程开关复测

运行：`build/runs/performance-combinations-20261106`。

启动固定：新原生字体、guest heap 1664 MiB、NVNTexture 子堆 512 MiB。
从 230 秒起，每 60 秒切换，顺序 `4,9,10,11,12,12,11,10,9,4`。
正式窗口为每段的第 20–55 秒，避开切换、热身和第 3 秒的窗口截图。
来回顺序用于检查收益是否只来自测试顺序、缓存变热或后半段变慢。

| mode | 管线/预算 | 日志减负 | 空闲页回收 |
|---|---|---|---|
| 4 | deep 64 MiB | 关 | 关 |
| 9 | deep 64 MiB | 开 | 关 |
| 10 | deep 64 MiB | 开 | 开 |
| 11 | deep 128 MiB | 开 | 开 |
| 12 | deep 128 MiB | 开 | 关 |

| 段 | mode | 整帧均值 ms | 帧数 | P95 ms |
|---|---|---:|---:|---|
| 1 | deep | 76.890 | 416 | 85–86 |
| 2 | deep-quiet | 74.486 | 448 | 82–83 |
| 3 | deep-compact | 74.060 | 448 | 83–84 |
| 4 | deep-expanded | 74.284 | 416 | 82–83 |
| 5 | deep-large-quiet | 74.933 | 416 | 85–86 |
| 6 | deep-large-quiet | 76.129 | 448 | 85–86 |
| 7 | deep-expanded | 76.117 | 416 | 87–88 |
| 8 | deep-compact | 76.904 | 416 | 87–88 |
| 9 | deep-quiet | 76.119 | 416 | 89–90 |
| 10 | deep | 78.787 | 416 | 90–91 |

| 合并模式 | 整帧均值 ms | FPS | 帧数 | P95 ms |
|---|---:|---:|---:|---|
| deep | 77.839 | 12.847 | 832 | 88–89 |
| deep-quiet | 75.272 | 13.285 | 864 | 86–87 |
| deep-compact | 75.429 | 13.257 | 864 | 85–86 |
| deep-expanded | 75.200 | 13.298 | 832 | 85–86 |
| deep-large-quiet | 75.554 | 13.236 | 864 | 85–86 |

判断：

- 日志减负：正向 76.890→74.486 ms，反向 78.787→76.119 ms，两对分别约 -3.13%/-3.39%；合并约 -3.30%，保留。
- 64 MiB 空闲页回收：正向约 -0.57%，反向约 +1.03%；合并 75.272→75.429 ms，未证明收益，关闭。
- 128 MiB + idle 相对 64 MiB + idle：正向约 +0.30%，反向约 -1.02%；合并差约 -0.30%，未证明收益。
- 都无 idle 时，128 MiB 为 75.554 ms，64 MiB 为 75.272 ms；大池未改善。最终保留 64 MiB，128 关闭。

真实容量也支持这个判断：新字体下，128 MiB 逻辑预算只需实际分配 **68 MiB** backing，
活子分配约 65.95 MiB。64 MiB 段实际分配 64 MiB，活子分配约 61.62–63.63 MiB；
正式窗口 pressure/nomem 均不增加，旧字体的大规模池压力已经消失。大池仍约每帧
一次驱逐，不能把所有驱逐都解释成容量不足。本轮关闭大池，减少的是 64 MiB 预算
上限和本场景约 4 MiB 实际 backing，**不是声称节省 64 MiB 已分配内存**。

10 个正式窗口共 4256 帧；逐窗口核对 root mode、quiet 状态、完整帧直方图和端点，
进程正常退出 0，存档及原 marker SHA256 恢复通过。原始数据与分析在运行目录
`combinations-report.json`；各 mode 的截图仍为同一页，NPC 自然动画不同。

注意：AB mode 名称 `deep-compact`/`deep-expanded` 是含 idle 的测试候选；最终
normal profile `compact`/`expanded` 已根据结果关闭 idle，分别相当于 AB mode 9/12。

## 二、正常启动完整组合复测

第一版把 deep 关闭、恢复 1895/850 MiB 作为基线，实际发现大规模绘制拒绝，已排除。
`performance-baseline-a/b-20261106` 的 132.842/145.033 ms 只保留为反例；A 窗口
448 帧中 79,424 次 g_fail，约 177.29 次/帧，整次日志有大量 stage 6/9 资源拒绝。
虽然窗口图看似基本正常，不能据此给出有效加速比。此前首对约 43% 的暂报比值作废。没有通过关闭拒绝
日志、绕过错误或丢弃绘制把这组数据改成有效。

保留的有效样本按 C/R/R/C：`compact-b`、`reference-a`、`reference-b`、`compact-c`。
R=上述 deep64/512/1664/原字体基线，C=最终 compact。首 C 与首 R 之间还运行过已否决的
legacy-B，四个有效样本不是连续无间隔运行；未控制宿主频率/温度和全部后台负载。
每次重启，235–300 秒窗口，
220/310 秒窗口截图确认相同对话页。另有 compact-a 75.742 ms 的较快样本保留作
额外观察，不加入这组按顺序的主汇总；较慢的 compact-b 82.897 ms 纳入主汇总。

| 按运行顺序 | 正式端点 s | 帧数 | 均值 ms | FPS | P95 ms | 成功 draw/帧 | 拒绝 draw/帧 |
|---|---|---:|---:|---:|---|---:|---:|
| compact-b | 235.914–299.579 | 768 | 82.897 | 12.063 | 98–99 | 441.01 | 0.025 |
| reference-a | 235.287–296.971 | 448 | 137.688 | 7.263 | 158–159 | 443.47 | 0.051 |
| reference-b | 235.130–298.210 | 448 | 140.805 | 7.102 | 165–166 | 442.64 | 0.020 |
| compact-c | 235.930–299.353 | 800 | 79.278 | 12.614 | 90–91 | 441.26 | 0.025 |

| 合并 | 帧数 | 均值 ms | FPS | P50 ms | P95 ms |
|---|---:|---:|---:|---|---|
| baseline | 896 | 139.246 | 7.182 | 136–137 | 162–163 |
| compact | 1568 | 81.051 | 12.338 | 79–80 | 95–96 |

两对帧时间分别下降 **39.79% / 43.70%**，方向一致。汇总按帧数和真实区间总时长
计算，不平均标题栏 FPS；共 2464 个正式帧间隔。四次均正常退出 0，原存档和 marker
SHA256 恢复通过。包/SELF/模拟器/index/manifest/sidecar 身份逐项一致，字体拒绝为 0。

同一端点的资源计数：

| 指标（每帧） | 原字体基线 | 最终 compact |
|---|---:|---:|
| 池驱逐 | 83.739 | 1.011 |
| 池压力事件 | 11.000 | 0.000 |
| 分配失败 nomem | 0 | 0 |
| kernel region 分配/释放 | 0 / 0 | 0 / 0 |

两侧池预算和实际 backing 均为 64 MiB。新字库显著减少反复驱逐和池压力；压力事件
是计数，不是 GPU 总等待时间。本轮关闭了细分计时，因此不把这些次数换算成未经测量
的 GPU 毫秒。字体、显示预设和日志同时变化，不能把 -41.79% 全部归给某一项。

成功绘制约 441–443 次/帧；两侧仍有平均约 0.02–0.05 次/帧拒绝，明确保留为未解决
范围。没有为了变快关闭拒绝/错误路径，也不宣称像素级全场景一致。`g_skip` 两侧均为
1 次/帧；不从计数推断全部玩法画面正确。新模式同时改善了字体和整幅画面的清晰度。

最终 `compact-c` 直接通过 `scripts/play.ps1 -RunnerOptions ...` 启动，未显式传 batch，
因此也覆盖普通入口默认 batch 的实际运行。完整结果和原始日志哈希在
`build/runs/performance-final-report-20261106.json`；较早的
`performance-normal-report-20261106.json` 已明确标记 invalid，不作收益依据。

compact-b 的输入辅助脚本在 305 秒误用 .NET MainWindow 选到模拟器的另一个窗口，
因此报错退出。它在 200 秒前已完成全部输入，正式计时止于 299.579 秒；独立 runner
在 310 秒正确选择游戏 HWND、确认 client=960×544 并拍到同一对话页，游戏正常退出，
存档恢复通过。该较慢窗口保留并注明范围。后续辅助脚本已改为按 PID 枚举有正确标题
的可见游戏窗口，源码编译检查通过；不隐藏原 error log。

## 三、成果如何处理

| 项目 | 证据与实际作用 | 本轮处理 |
|---|---|---|
| RG32F 顶点数据纹理、修正本地 OpenGL 点积顺序 | 修复黑块/角色错位；已验顶点位置。它们是正确性修复，不靠减少画面工作量提速 | 两侧保留，不再回到有精度错误的旧包 |
| 有界 batch、Scene 流水线、clear fusion | 既有正确默认路径；资源完成条件保留。不同于 mode 6 的额外 deferred-clear 实验 | 保留 |
| 已比较的 pair 提示及通知等待归并 | 完整内容比较保留，只复用已验证结果、等待最新完成点后安全退役旧资源 | 保留；没有本轮独立累计收益数字 |
| SDK 轻量锁、Session 用户态门锁、原生视频表面按行读回 | 既有平台层改进，错误和递归语义保留 | 保留；本轮两侧一致，不把历史局部毫秒相加 |
| decoder-owned deep 纹理链路 | 原先有限对照约 4.9%；转换结果直接供 GXM，逐字节 raw 身份、pin/fence 不变 | 保留；本轮作为正确基线两侧共同路径，不重复计入总收益 |
| Alpha8 字体 | 常用 10 张源字体页 decoded 40→11 MiB，额外字模元数据约 1.64 MiB；boss 两组另各 1 MiB，不能把不同语言/加载阶段当成同时驻留 | 保留经过验证的单通道路径 |
| 按实际字号选字模、直接落屏幕像素 | 8/10/12/16px 原生点阵档，常用中文字形 16px；修正原游戏 1080p 最大缓冲区配 720p 视口的二次缩放。101 个实际字形 quad 和 3 次 VI copy 已验 1:1 | 保留清晰度修复；不能单独从图像证明提速 |
| 512 MiB 原纹理子堆 + 1664 MiB guest heap | 原 850 MiB 子堆预留挤占后端；旧字体下真正取得完整 128 MiB 后，整帧约 -33.7% | 作为容量候选；仍只有有限 L3，任务/流式峰值待验 |
| 正常工作日志减负 | 旧窗口约 2.0%，新字库往返合并约 3.30%；保留错误与证据日志 | 开启 |
| 空闲异尺寸页回收 | 旧工作集下约 5.25%；本轮方向不一致，合并略慢 | 关闭 |
| 128 MiB 统一池 | 旧字体下曾有效；新字体实际只需 68 MiB backing，扩池不再改善整帧 | 关闭；仅 expanded 保留复测入口 |

旧百分比都来自各自受控窗口，不代表本轮结果。原记录见
[安全迁移](13-render-performance-lessons.md)、[成本审计](16-frame-cost-audit.md)、
[预算验证](17-texture-memory-validation.md)、[原堆与生命周期](18-resource-lifecycle-and-capacity.md)、
[Alpha8](19-pixel-fonts.md) 和[原生字模](20-native-pixel-fonts.md)。

## 四、关闭的实验和诊断

| 选项 | 关闭依据 |
|---|---|
| prepared vertex layout | 两轮未证明整帧收益 |
| 完整 C prepare / fast / 原游戏 root 接管 | 路径能运行，但同样的资源处理和 GXM 工作仍在，没有稳定净收益 |
| shared prepare | 约 0.71% 小变化不足以证明收益 |
| mode 6 deferred clear | 有限窗口约慢 0.98%，未解决主要成本 |
| mode 8 cached raw / decoded 拆分 | 有限稳态约慢 12.46%，直接启动还有加载失败 |
| 原 NVNTexture 子堆 256 MiB / guest 1408 MiB | 已观察到越界映射拒绝和明显破图 |
| 跳过 raw 字节比较、延迟校验、概率 hash、阴影跳帧 | 没有足够内容失效/画面时效证据，不能作为安全收益合并 |
| native movie | 完整视频帧率已验证，但没有音频；不进入正常推荐、不计游戏提速 |
| play/game/frame-cost、资源/字体/skin trace、输入记录、dump | 只用于定位问题；正常推荐全部关闭 |
| 旧锁/Session/读回回退 marker | 正常推荐移除，保持已采用的快速路径；需要单项调查时再手动开启 |

运行目录还留有 7 个旧分支 marker：frame-time、nvn-hook-record、phase-census、
present-counter、tex-copy-census、texture-validate-interval、texture-validate-once。
当前 src/tools/scripts 已无读取逻辑；本轮纳入 managed 集合，备份后移除，避免误以为
它们仍控制当前版本。

关闭实验入口不等于删除原研究证据。保留历史提交和分析工具，生产推荐不再依赖实验
控制文件；不清理用户存档、原游戏包或旧证据目录。

## 五、配置与复现

`config/performance-profiles.json` 是 marker/文本配置的唯一数据源。

| profile | 字体 | deep | 日志减负 | idle | 池预算 | 原纹理子堆 / guest heap |
|---|---|---|---|---|---|---|
| baseline | 原 RGBA | 开 | 关 | 关 | 64 MiB | 512 / 1664 MiB |
| compact / recommended | 原生点阵 Alpha8 | 开 | 开 | 关 | 64 MiB | 512 / 1664 MiB |
| expanded | 原生点阵 Alpha8 | 开 | 开 | 关 | 128 MiB | 512 / 1664 MiB |

`expanded` 仅保留作容量重测。三者都关闭 C root 接管、shared、deferred-clear、split
和诊断探针，使用 540p / GPU present / bounded batch。`baseline` 是有效对照配置，
不是恢复原安装所有字节；精确回滚应按备份 manifest 恢复。

```powershell
# 先看会改什么，不写任何文件
./scripts/set-performance-profile.ps1 -Profile recommended -DryRun

# 关闭 Vita3K 后应用；原配置逐文件备份到 local/performance-profile-backups/
./scripts/set-performance-profile.ps1 -Profile recommended

# 需要重测时显式切换
./scripts/set-performance-profile.ps1 -Profile baseline
./scripts/set-performance-profile.ps1 -Profile compact
./scripts/set-performance-profile.ps1 -Profile expanded

# 普通启动默认使用受测 batch；可传入有类型的 runner 参数
./scripts/play.ps1
./scripts/play.ps1 -RunnerOptions @{ NoDetach = $true; ExitAfterSeconds = 330 }

# 已有受保护运行的窗口统计
./build/python-env/Scripts/python.exe tools/diagnostics/measure_frame_window.py `
    <run-dir> --start 235 --end 300 --output <result.json>
```

`recommended` 是选择配置的别名，不改变没有 marker 时的二进制默认值。
脚本拒绝活 Vita3K，只操作 JSON 指定的文件，预先检查所需 font-native 包；
保存原始字节、存在状态、SHA256、配置收据。未知文件、存档、eboot 和字体载荷不由
该脚本修改。包安装另记原 eboot 备份和新 SELF 哈希。

本地最终状态（2026-11-06）：

- `build/vita/` 默认 VPK/SELF、安装 eboot 已选择上述受测哈希，compiled shader metadata
  与包匹配；默认选中的 RG32F 包完整校验通过（364 GXP/variants）。
- `recommended=compact` 已实际应用，67 个 marker + 2 个文本文件逐项核对；旧无效
  marker 已移除，原输入邮箱与存档保留。当前未留下运行中的 Vita3K。
- 原 VPK/SELF/metadata/eboot 在 `local/performance-install-backups/20261106-final/`；
  安装收据为该目录的 `installation.json`。
- 原配置逐字节备份：`local/performance-profile-backups/20261106-120451-902a9705/`。
  manifest 保存原存在状态和 SHA256；精确回滚按该清单复制已有文件、移除原先不存在
  的 managed 文件，再恢复原 eboot/VPK/SELF。回滚时先关闭 Vita3K。
- 正常启动汇总：`build/runs/performance-final-report-20261106.json`，SHA256
  `46ab4afd71579d009fbae63180a6a25cf9b2e5c1ca97e0fc4c5168742553dba2`。
- 字体截图整数 2 倍对照：`build/runs/performance-visual-review-20261106/font-comparison-2x.png`；
  原始完整窗口在 reference-a / compact-c 的 220 秒 PNG 中。只作图像核验，不作计时来源。

`play.ps1` 原来不传 `-BoundedBatch` 就会被底层 runner 暂时移除 marker，导致配置与
实际启动不一致。现在普通入口默认传入 batch；`RunnerOptions` 用 hashtable 保持
switch、数组等类型，需要诊断时可明确传 `BoundedBatch=$false`。底层 runner 的
marker 恢复、进程归属、截图、正常退出和错误检查均保留。底层失败继续抛异常；
不再把之前某个 native 命令的旧 LASTEXITCODE 当成本次 PowerShell 脚本的退出值。

## 证据级别和未验证范围

本轮源码/配置为 L0，61 项宿主回归为 L1，Vita 构建为 L2，实际运行结论限定到 L3。
font-native 包与本地构建输出的 145 个文件逐个 SHA256 一致（含许可/元数据），不改写字体载荷。
不外推 PS Vita 真机速度、可用物理内存和缓存行为；L4/L5、任务/住宅、完整战斗回村、
切语言/字体流式峰值以及 1–2 小时稳定性仍未验证。未宣称达成 30 FPS。
