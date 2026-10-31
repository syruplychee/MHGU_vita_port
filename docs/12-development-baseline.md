# develop 开发起点（2026-11-01）

## 旧项目交接

### 范围与结论

旧工作区：`C:/Users/15196/Desktop/workspace/MONSTERHUNTER/vita-port`。
本次读取时旧仓库 HEAD 为 `6aced0835dd820510e88c9fa63cf9b158e74bbca`，但存在大量
已修改和未跟踪文件；**这个提交号不能代表旧工作区的全部现状**。本节以实际文件为准，
只读审查，没有复制旧工作区未提交代码、游戏文件、存档或运行生成物。

明确判断：旧项目已做到在特定 Vita3K OpenGL 版本中启动原始游戏、显示中文、读写
存档并进入可见村庄；仍未做到稳定可玩的完整单人任务闭环，也没有真机验证。
最新研究推进到 B130，但新仓库源码只包含其中一部分，不能把“旧项目最新结论”写成
“新仓库已具备全部修复”。

本次交接审查本身是 **L0（报告与源码核对）**。下表 L3 指旧报告绑定的历史 Vita3K
运行，不是本次重新执行；新仓库本次构建、测试与运行结果应单独记录。证据定义沿用
[测试与证据规则](08-testing.md)：L0 静态、L1 宿主测试、L2 构建、L3 Vita3K、L4 真机、
L5 完整玩法验收。

### 已证实到哪里

下列旧文档路径均相对于旧工作区根目录。

| 能力 | 历史证据与出处 | 仍未证明 |
|---|---|---|
| 四模块加载、原始 SDK 初始化、进入游戏 | L3；旧 `docs/PROGRESS.md`，新仓库 [架构](01-architecture.md) 可对照实现 | 真机模块映射、权限、cache 与线程行为 |
| 创建角色、真实写档、关闭后由新进程读回同一角色 | L3；旧 `docs/M6_A_SAVE_RELOAD_20261026.md`：ABC 角色、两次不同 PID，写入/Flush/Commit、完整读回与原档恢复核验 | 完整任务奖励保存、所有失败恢复与真机存储行为；该轮退出需要强制停止 |
| 中文界面、村长对话、村庄地形/建筑/人物/投影 | L3；旧 `docs/M6_B116_FAST_RENDER_20261031.md`，B118 报告补充自由移动观察 | 全地图、所有材质、长时间动态画面正确性 |
| 住宅与任务 | L3 局部；B119 报告记载 B118 人工会话住宅正常，任务仍全黑、只有 HUD；B121 未进入任务内容 | 任务画面恢复、战斗、结算、回村与跨启动保存闭环 |
| 视频解码与合成 | L3；B123、B125–B130 报告 | 正常播片节奏与音画同步；内容仅约 2.7–2.9 FPS |
| 音频服务与混音 | L1–L3 局部；旧 `docs/PROGRESS.md` 和新 [当前状态](09-status.md) | 真机连续输出、时钟、欠载与生命周期稳定性 |
| 静态 shader 包 | L2/L3；B118 的 2,402 stage / 364 variant 包构建并投入会话 | 所有场景 shader/纹理/深度覆盖；构建成功不是像素验收 |

没有新增 L4 或 L5 证据。某次人工进入住宅、看到村庄或读回角色，不能替代完整玩法验收。

### B118–B121：任务黑屏没有关闭

1. **B118** 修正颜色目标表 32 槽永久耗尽，改为有界 LRU；只解析 shader 实际使用的
   主采样器；静态包由 293 补到 364 variant。自动回放只证明到达村庄，没有进入任务，
   也没有触发 32 槽回收，故 LRU 回收仅有定向宿主证据。
2. **B119** 将片元采样器上限从 4 扩到 5。实际会话里五采样器拒绝归零，整轮
   `draw_fail` 从旧会话的 215,073 降至 12,727；**任务世界仍黑，任务段约 6 FPS**。
   两轮阶段和时长不同，拒绝次数不能当成受控改善比例。
3. **B120** 试图按 clear 颜色位筛选颜色生产者，既没解决 256×1 U16 目标，又引入
   人物纹理错位、闪烁和黑点；该方案随后撤销，不能当成有效修复继续合入。
4. **B121** 改为 clear/draw 被 `guest_gxm_request` 接受后才记录颜色生产者，draw
   仍要求非零颜色写掩码。村庄/住宅会话 `extra_missing=0`，但七个任务五采样器 stage
   都未加载，**没有任务内容验证**；用户仍报告纹理错位/闪烁。256×1 目标还不能创建
   GXM 颜色面，走既有 CPU 解码路径。

出处：旧 `docs/M6_B118_SCENE_RENDER_20261031.md`、
`docs/M6_B119_FIVE_SAMPLERS_20261031.md`、`docs/M6_B120_CLEAR_PRODUCER_20261031.md`、
`docs/M6_B121_ACCEPTED_PRODUCERS_20261031.md`；相关旧 ADR 0114–0117。
任务黑屏、纹理闪烁与零散拒绝仍是下一次定向运行需要确认的问题。

### B123–B130：视频锁路径已有结论，播片仍慢

**不要再把这件事仅写成“怀疑某把锁阻塞，待 B125 计时”。旧研究已经定位到锁内的
`NvRmMemReadStrided` 逐行读回/转换路径。** 这个结论来自旧模拟器测量，真机仍未知。

- B123：原始片头素材为 29.97 FPS，实测提交间隔 352.3 ms，约 2.84 FPS。
  `sceAvcdecDecode` 平均 5.3 ms，VIC 合成约 0.43 ms；解码与合成钩子不是主要耗时。
- B125：修复服务停驻后恢复了其他线程的句柄/TLS 的问题，停驻前保存本线程环境，
  重取会话锁后恢复；这既影响诊断归属，也影响服务 owner 判断。
- B126/B127：锁等待切片改成 1 秒、保留总计 5 秒超时报错；阻塞前最多 256 次、约
  1 ms 的 TryLock 自旋，避免频繁重排队与短临界区的模拟器调度成本。
- B128：递归进入/退出仅更新 guest 深度，宿主锁只在最外层获取/释放；增加 64 项
  地址提示缓存。历史解码间隔从 B125 的 483 ms 降到 B128 的 342 ms。
- B129：持锁栈与服务计数把剩余时间指向播放器在引擎锁内执行
  `NvRmMemReadStrided`，反复按行调用 `NvRmMemMap` 和锁服务；解码线程在同锁等待。
  播放器没有对应的长睡眠、键等待或 IPC 等待，不能简单归为桥接层死锁。
- B130：无争用 Enter 直接 TryLock，服务单次耗时约 169→58 µs，但解码间隔仅
  375→366 ms，每帧持锁约 342→339 ms，基本未变。**锁调用更便宜没有带来正常播片**。

旧报告最终判断：在该 Vita3K 环境里，剩余约 340 ms/帧主要落在 guest 的视频表面
逐行读回/转换与相关 trap/模拟器调度。后续应评估原生平台读回实现，或用真机测量
检验这个路径；不要继续把“压低桥接锁时间”当成足以达到 30 FPS 的方案。
这不是宣布所有锁问题已消失，也不是已经完成原生读回替换。

出处：旧 `docs/M6_B123_MOVIE_PACING_20261031.md`、
`docs/M6_B124_MOVIE_BRIDGE_TRACE_20261031.md`、
`docs/M6_B125_B130_MOVIE_LOCK_PACING_20261031.md`；旧 ADR 0118–0121。
历史会话记录均注明存档 preserve/restore/verify 失败为零；本次没有重跑这些会话。

### 新仓库到底包含哪些

核对基点：新仓库初始提交 `60eee23` 的生产源码，与 2026-11-01 读取的旧工作区文件。
以下均是 **L0 代码比对结果，不代表这些功能已经用本次新 VPK 逐项运行过**。

| 旧项目内容 | 新仓库状态 | 具体核对 |
|---|---|---|
| B118 场景目标与 GXM 后端 | 已包含主要代码 | `src/platform/gxm_runtime.c` 与旧文件的唯一差异是默认静态包路径改为 `static-shaders-current` |
| B119 五片元采样器 | 已包含 | `src/platform/gxm_sampler_slots.h` 中上限为 `5u`；相关 guest 循环使用同一宏 |
| B121 接受后标记颜色生产者 | 已包含 | `src/platform/guest_svc.c` 两库文件相同；clear 在 `rc == 0` 后记录，draw 还要求 `req.color_mask != 0u` |
| B124 媒体钩子计时/线程归属 | 已包含相同媒体文件 | `src/platform/media_video.c` 两库文件相同 |
| B125 停驻后还原句柄/TLS | 已包含 | 新 `src/loader/sdk_dso.c` 的 `park_begin/park_end` 对应旧 `src/probes/native_sdk_dso.c` |
| B126 1 秒等待切片、5 秒总上限 | 已包含 | 新 `sdk_dso.c` 的 `VP_SDK_LOCK_SLICE_US=1000000u`，Enter 超时失败路径仍在 |
| B127 阻塞前有界自旋 | **未包含** | 新 Enter 和键等待重取路径直接走阻塞循环，没有旧代码的 256 次/1 ms TryLock 路径 |
| B128 递归锁快路、host_depth、64 项地址提示缓存 | **未包含** | 新代码没有这些字段/快路；键等待仍按递归深度多次释放、重取宿主锁 |
| B129 服务 top-8 与长持锁栈探针 | **未包含** | 新文件没有 `VP_THREAD_TOP`、`VP_HOLD_STACK` 与相关统计/采样函数 |
| B130 无争用锁直接获取与计时门控 | **未包含** | 新 Enter 没有 `depth==0 && pending==1` 快路，开始计时仍直接调用宿主时间函数 |

新 `sdk_dso.c` 与旧 `native_sdk_dso.c` 的差异不只是改目录和头文件名，不能整文件覆盖。
后续若引入 B127–B130，应审查 Enter/Leave、键等待释放/重取与深度记账的整套变化，
补对应宿主验证、构建和受保护运行，再记录 ADR；本次交接不自动引入。

### 性能、真机与建议起点

- 村庄：旧 B116 同脚本 260–310 秒窗口为 **6.6174 FPS、150.64 ms/帧**，通常
  可概括为 540p 约 5–7 FPS；这是特定模拟器、特定场景窗口的观测。
- 任务：旧 B119 人工会话任务仍黑、窗口约 **6 FPS / 168 ms**。其整轮平均约 25 FPS
  混有菜单和加载，不能写成任务已达到 25 FPS。
- 播片：最新旧会话仍 **2.7–2.9 FPS**，不能用渲染 present 频率替代内容更新率。
- 模拟器依赖：旧 B116 使用基于 4106/d7edcbe9 的 B64 修正版；当时原版
  0.2.1 4236/17a8b340 在 VM 分配阶段失败，未进入游戏。新版本运行能力应重新测量，
  不能继承旧修正版证据。
- 真机：没有 L4。大模块 RX/R/RW/NX 映射、cache、峰值内存、真实 GXM、音频、
  显示与休眠均待测；模拟器可运行不能证明内存装得下或真机也有同样速度。
  检查顺序见 [真机清单](07-hardware.md)。

接下来建议：先用本仓库构建与一次受保护 Vita3K 会话固定可重复的开发起点；随后
优先复测任务黑屏/人物纹理闪烁，并按真实拒绝日志修图形路径。视频方向直接沿用
B129/B130 的读回归因，避免重复从解码器或未知锁开始排查。B127–B130 未迁移项单独
审查，不把它们混进图形修复或本次环境打包验证；真机从映射/cache/内存准备开始。

本次曾通过 `claude-delegate` 与 `codex-background` 发起 240 秒只读辅助审查；委派
超时，未得到最终报告，只读检测未发现改动。本文结论由 Codex 重新阅读上述旧报告和
实际源码并逐项比对，不把委派退出或旧报告标题当作验收通过。

## 本仓库重新验证的开发环境

### 分支、工具与构建

- `develop` 从 `60eee236b0888633a91839c8b91860f5f2786522` 创建；本次未迁移旧代码。
- Python 使用 `uv` 创建的 `build/python-env`：Python 3.12.13、lz4 4.4.5。
- VitaSDK GCC 15.2.0，SDK 构建日期 2026-08-25；Ninja 1.13.2；CMake 4.3.1。
- 宿主测试使用 MSVC 19.51.36252、Visual Studio 18 2026、Release，**26/26 通过（L1）**。
  仅 POSIX 的四项本次未运行。MSVC 仍有既有类型转换/可能未初始化警告，未在本次扩大修复。
- ExeFS 四模块、NPDM、NACP 输入身份全部通过（L0）；本次未完整扫描 RomFS 内容。
- 静态包校验通过（L0）：ABI 2、2,402 stage、364 variant、6,311 program、364 GXP。
- VPK/SELF 重新构建成功（L2），未关闭身份校验、断言或失败路径。

本机 CMake 4.4.3 在 D 盘无法创建 `pkgRedirects`。换成 4.3.1 后，GCC/Ninja 仍无法
向 D 盘和本机 `.codex/builds` 写临时文件。实际通过的独立构建目录为
`C:/Users/15196/Desktop/workspace/MONSTERHUNTER/build/mhgu-vita-develop`，源码仍是本仓库。
构建后将 VPK、SELF 和静态包路径描述复制到 `build/vita/`，供默认运行入口使用。
这不修改旧 `vita-port` 工作区，也没有把旧产物当作本次编译结果。

构建产物身份：

| 对象 | SHA256 |
|---|---|
| 输入锁 `config/target-1.4.0.json` | `5b5da067a6a5aa786e53c4970e09f6945bd6e697c1f2bfebd070cf0b65aee3f8` |
| 本次 VPK | `be706cf27fa532f48820f811e3eebe27a3734538c1c70a9771b4f120893ec90d` |
| 本次 SELF / 安装后 eboot | `d6d210cdbaa2ac6df8b1827d5adfceda7e65dc7215f6715c8d146a6db1ed7a79` |
| Vita3K | `d21795e11ecfa4ec6f9efee4e727cfcd55ef27b0a0c27ea44aa98b4a9cbc9b6f` |
| 静态包 manifest | `f7f49f56cd0642ec8282c2b07c192b0af0fa179b374220fcc363939cfea6e285` |
| 静态包 index | `f003f35a009153d416763dbad6a5834182af98cb7f5a99dc8c5141b00f873df3` |

SELF 与初始化文档所记录版本相同；VPK ZIP 时间戳会使容器哈希变化。

### 本地复现入口

机器路径已写入被 Git 忽略的 `local/develop-env.ps1`。在仓库根目录执行：

```powershell
. .\local\develop-env.ps1
.\scripts\check-inputs.ps1
.\scripts\host-tests.ps1 -CMake $env:MHGU_CMAKE
.\scripts\build.ps1 -BuildDir $env:MHGU_BUILD_DIR -CMake $env:MHGU_CMAKE
Copy-Item "$env:MHGU_BUILD_DIR/vita_port_native_loader.vpk" build/vita/ -Force
Copy-Item "$env:MHGU_BUILD_DIR/vita_port_native_loader.self" build/vita/ -Force
Copy-Item "$env:MHGU_BUILD_DIR/static-shader-package.json" build/vita/ -Force
.\scripts\play.ps1 -RestoreAfter
```

最后一条是手动操作的 GUI 会话，关闭后恢复启动前存档。默认运行配置保持 540p、
OpenGL、SDL 音频；实验性批处理需显式选择。本次自动化检查另外启用了
`-BoundedBatch -PlayPerf`，因此不能把本次观测当成默认无批处理路径的性能数据。
本地 `build/run-develop-baseline.ps1` 保存了本次带测试存档、输入回放、截图与恢复的命令。
这些本地文件和运行依赖不会随 Git 在另一台机器上自动出现。

模拟器 EXE/DLL 已复制到独立的 `build/vita3k-runtime/`，避免改写旧模拟器目录里的
配置、缓存和日志；界面资源使用只读用途的 junction。原有合法固件复制到新 pref，
RomFS 继续使用既有 junction，未另复制约 12 GB 游戏树。

### 发现并修复的存档入口问题

`scripts/backup-saves.ps1` 原先通过 `Write-Host` 打印备份回执，但 `play.ps1` 用标准
输出捕获并提取备份路径，所以实际备份虽存在，`-RestoreAfter` 可能取不到路径而不恢复。
已将最后两行回执改成 `Write-Output`。独立哨兵文件检查确认：能够捕获路径、修改文件后
恢复、恢复前后 SHA256 相同；实际运行的原始存档恢复结果另见下节。

### 发布包装限制

额外运行 vita-port 技能的 `check_vpk.py`：SELF 头、SFO、Title ID 与版本可解析，但包内
只有六项，没有 icon、LiveArea 背景、startup 图和 template XML，检查报四项错误；
另有非标准 Title ID 命名样式提示。这是现有开发包的发布包装缺口，不能称为发布包。
本次验证只判断新源码能否构建并在所选 Vita3K 中运行，没有通过删检查来让此项变绿。

### Vita3K 实际运行结果（L3）

首轮证据目录：`build/runs/develop-baseline-20261101/`。

- 时间：2026-11-01 00:13:24–00:19:12（北京时间）；进程生命周期 347.203 秒，
  配置运行上限 330 秒，另有窗口准备与正常关闭时间。
- 模拟器：日志版本 `0.2.1 4106-d7edcbe9`，既有 B64 修正版；OpenGL 初始化核验通过。
  驱动日志为 NVIDIA RTX 5060 / OpenGL 4.6 / 610.88。不能将其等同原版最新版或真机。
- 配置：540p、SDL 请求、`BoundedBatch`、`PlayPerf`、无静音诊断，未开启 SurfaceReadback。
  本次未核验实际听感；回执未确认音频后端初始化，不能宣布音频通过。
- 90 秒真实窗口截图显示中文存档选择、角色 A/ABC 与 3D 人物模型；230/310 秒截图
  仍在村庄加载画面，左下角出现 shader 编译数量。它们**不是村庄截图**。
- 用户随后明确反馈“已经进入村庄，然后手动退出了”。因此本轮进村依据是用户现场
  确认与实际模拟器会话，记录为 L3；没有把这句话扩大为完整任务 L5 验收。
- 末次静态统计：`draw_ok=2675`、`draw_fail=58`、`batch_encoded=165632`、
  `batch_failed=0`、`batch_max=123`、`batch_uniform_peak=129232`。这些是整轮累计，
  混有菜单/加载与村庄，58 次 draw 失败仍未在本次逐项关闭，不能称为图形完全通过。
- 进程回执：`graceful_exit=true`、`exit_code=0`，未强杀。用户退出游戏后，runner
  对其拥有的前端窗口发送正常关闭请求。自动截图与前端生命周期检查均通过。
- 原存档目录本来为空。将旧 B130 已核验哈希的中文测试存档**复制**到新 pref 的独立
  测试目录后运行；旧归档与用户原档不修改。结束后移走本次写档，恢复原空目录，
  `save-protection.json` 记录 `restored=true / verified=true / file_count=0`。

首轮因为独立 pref 的加载耗时较长，310 秒仍未得到村庄截图，曾额外启动补充复测：
`build/runs/develop-baseline-warm-20261101/`，仅该轮另外开启 `SurfaceReadback`。
收到用户已进村并退出的反馈后，正常关闭此补充进程，不再等待整套截图；其进程退出
码为 0、存档恢复校验通过，但 runner 明确报 `capture_failure=process-exited`。
这轮是**主动提前结束的未完成复测**，不计为额外通过的验收，也不用于比较快慢。

判断：源码构建、宿主测试、模拟器启动、中文存档选择、用户确认进村及正常退出的
开发链路已经建立，可以在 `develop` 继续开发。首次加载慢需要后续单独测量，不能
仅凭 shader 编译提示断定全部耗时来自 shader；本次也没有重测村庄稳定 FPS。
任务场景、视频节奏、实际音频、1–2 小时稳定性、真机及发布包装仍未完成。
