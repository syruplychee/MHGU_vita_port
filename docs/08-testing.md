# 测试、证据分级与验收

## 1. 五类证据

任何结论必须标注它属于哪一类；低层级证据不能冒充高层级：

| 级别 | 证据 | 能证明什么 | 不能证明什么 |
|---|---|---|---|
| L0 静态审计 | 源码/二进制/契约检查 | 结构、身份、边界条件 | 运行、像素、性能 |
| L1 宿主测试 | `tests/` 在 PC 上运行 | 纯函数/策略/解析器行为 | Vita API、GXM、真机 |
| L2 构建 | VPK/SELF 链接成功、哈希 | 代码可编译、产物身份 | 运行成功、画面正常 |
| L3 Vita3K | 真实模拟器运行、截图、日志 | 该版本模拟器下的行为 | 真机行为、硬件性能 |
| L4 真机 | PS Vita 实机运行 | 真实硬件行为 | 完整玩法仍需 L5 |
| L5 玩法验收 | 用户实际操作完整流程 | 可玩性 | 不替代性能/稳定性数据 |

构建成功、单次截图、单个 draw 通过都不能写成“移植完成”。

## 2. 宿主测试

### 2.1 运行

Windows：

```powershell
.\scripts\host-tests.ps1
```

默认构建 Release；CMake 用 `/UNDEBUG`/`-UNDEBUG` 保留断言，避免大状态测试在 Debug
下栈溢出。也可以显式选择：

```powershell
.\scripts\host-tests.ps1 -Config Debug
```

Linux：

```bash
cmake -S tests -B build/host-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-tests -j
ctest --test-dir build/host-tests --output-on-failure
```

Windows 需要 Visual Studio（C 编译器）或可用的 Ninja + clang/GCC 环境。
CI 使用 Ubuntu + GCC。

### 2.2 覆盖范围

跨平台测试（29 项，需要 Python 3.10+）：

- `audio_downmix`、`audio_ipc`
- `batch_policy`
- `clear_fusion`
- `float_bits_numeric`
- `gpu_command_buffer`、`gpu_effective_rt`、`gpu_indices`、`gpu_stream`、
  `gpu_texture_decode`、`gpu_uniform_state`
- `gxm_depth`、`gxm_depth_view`、`gxm_depth_view_shadow`、`gxm_render_pipeline`、
  `gxm_resource_layout`、`gxm_sampler_slots`、`gxm_shadow_sampler`
- `memory_attributes`、`nvmap_table`
- `media_strided_copy`
- `pair_cache_policy`、`render_resolution`
- `static_shader_index`、`texture_snapshot`、`time_calendar`
- `vertex_format`、`vertex_window`
- `vertex_rg32f_lowering`（Python）：顶点浮点纹理展开、逻辑尺寸与片元路径边界

仅 POSIX/Linux（4 项）：

- `audio_auto_catchup`（`sys/mman.h`、`MAP_32BIT`）
- `audio_wave_progress`（`MAP_32BIT`）
- `guest_storage`（32 位 guest 地址 + POSIX mmap）
- `log_sink`（pthread）

这些测试直接编译生产源文件或包含生产实现，不是复制品。`gxm_render_pipeline`
由 `tests/CMakeLists.txt` 用标记从 `src/platform/gxm_runtime.c` 截取实际函数体生成
测试 include；标记缺失会在 configure 阶段失败，避免静默测试一份复制实现。

仅 Windows 另有 `session_gate`：使用真实 Win32 信号量和八线程，检查共享门锁的
互斥、递归、先释放后等待、过期唤醒 token 与错误传播。当前 Windows 共 30 项。

附加的播片计时工具测试：

```powershell
python -m unittest discover -s tests -p test_movie_timing.py -v
python tools/runtime/measure_movie.py --log <sdk-dso.log> --start 45 --end 100 --output <report.json>
```

工具按成功解码/合成的计数和时间计算内容处理速率，重复计数不增加帧数；日志缺口会
明确标记。Y/UV 两次读回只是两份平面复制，不能算两帧。这个结果不单独证明显示或音频。
测正常游玩性能时不要添加 `-SurfaceReadback`；该参数会开启额外 GPU 表面同步，
适合检查 CPU 读回数据，应与正常配置分别报告。

颜色转换探针（仅 `media-color-probe.enable` 存在时有效）：

```powershell
python -m unittest discover -s tests -p test_movie_color_timing.py -v
python tools/runtime/measure_movie_color.py --log <sdk-dso.log> --output <color-report.json>
```

工具只汇总探针完成的 `VP_MOVIE_COLOR_TIME` 记录，并按 rgb/nv12 分开统计。`elapsed_us`
是入口到出口的墙钟时间，包含 guest 调度与桥接，不是纯 CPU 时间，也不能当成帧率；
探针在前 2000 次完成后改为每 32 次记录一次，日志行数不等于转换总数。报告会明确标记
采样区间，并在探针未启用、补丁被拒绝或没有命中时给出原因，不能把“无样本”读成零成本。

框架级原生播片（仅 `native-movie.enable` 存在时有效）：

```powershell
# 先创建 ux0:data/mhgu-vita-port/native-movie.enable。
# 完整片头约 176 秒，窗口要覆盖首次解码到标题出现。
.\tools\runtime\run_gui_play.ps1 `
  -Vpk <vita_port_native_loader.vpk> `
  -OutDir build/runs/native-movie-full-<date> `
  -CaptureSeconds 60,120,180,225 -ExitAfterSeconds 240
```

验收日志是 `VP_NATIVE_MOVIE_DONE decoded=... presented=... elapsed_us=...
rate_milli=... failed=0`。必须同时报告影片文件、VPK/SELF 哈希、是否有音频；`presented`
是原生呈现帧，`decoded` 是送入解码器的 AU，两者不能互换。`rate_milli` 是
`presented × 1e9 / elapsed_us` 的千分率；本机完整片头为 5278/176085450 →
29974，即 29.974 内容帧/秒。`native-movie.limit-ms` 只用于短诊断，不能拿有限窗口
冒充完整片头。任何 `VP_NATIVE_MOVIE_FAIL` 都表示已回退 guest 管线，不能记为成功。

这项验收是 L3，且当前没有音频：原生呈现期间 guest 音频被静音。MP4 解析/呈现代码
没有独立的宿主单元测试，首次验证依赖 movie06 的完整 L3 运行；在补充解析器宿主
测试和真机 L4 前，不得写成 L1/L4 或“移植完成”。

SDK 锁控制流程的 Windows 模拟检查（需要 MSVC）：

```powershell
python tests/sdk_lock_regression.py --output build/sdk-lock-check
cmake -S build/sdk-lock-check -B build/sdk-lock-check/build
cmake --build build/sdk-lock-check/build --config Release
ctest --test-dir build/sdk-lock-check/build -C Release --output-on-failure
```

输出目录必须是新目录。脚本从生产实现提取代码，27 组检查使用模拟互斥体/事件，
验证递归、owner、快慢混合、key wait、超时、条件变量当前等待者语义、两种 Session
后端和真实错误；不能替代真实内核并发测试。

### 2.3 测试约定

- 测试必须可重复、无网络、无游戏输入、无真实存档。
- 使用 `assert` 或显式失败计数；释放构建也要保留断言（CMake 用 `/UNDEBUG`、`-UNDEBUG`）。
- 新逻辑优先做成纯函数/策略头，便于宿主覆盖。
- 涉及 Vita SDK 类型的路径用最小 stub 或独立探针，不把 Vita 头引入宿主测试。
- 失败修复后，测试名称/期望值要与当前契约一致；不要为了通过而删除检查。

## 3. 输入与构建校验

每次发布或 PR 至少：

```powershell
.\scripts\check-inputs.ps1
.\scripts\build.ps1
Get-FileHash build/vita/vita_port_native_loader.vpk -Algorithm SHA256
```

记录：

- 提交哈希
- `config/target-1.4.0.json` 的 SHA256
- VitaSDK/GCC/CMake/Ninja/Python 版本
- VPK/SELF SHA256

Configure 阶段已经验证 SDK 契约头；构建成功不代表运行成功。

## 4. 静态着色器包校验

```powershell
python tools/shader/verify_static_package.py <package-dir>
```

通过只代表包完整性（manifest、索引、GXP、sidecar），不代表 GXM 链接、像素或性能。

## 5. Vita3K 验收

一次有效运行至少记录：

- Vita3K 可执行文件 SHA256、后端（必须是 OpenGL）
- VPK/SELF/eboot SHA256、静态包 manifest/index 哈希
- 分辨率、marker、运行时长、退出码
- 截图时间点与窗口状态（可见/前台/尺寸）
- 日志中的关键计数：draw、batch failed、拒绝原因、帧时间
- 存档 preserve/restore/verify 结果

判据按任务而定。例如“村庄可见”需要真实游戏窗口截图；“性能改善”需要同一场景窗口
的帧时间分布，不是单帧 FPS。

## 6. 真机验收

按 [真机清单](07-hardware.md) 执行。每项需要：

- 可复现命令/构建
- 判据
- 原始日志与截图（本地）
- 环境卡
- 与模拟器差异记录

真机结果与模拟器不同时，以真机为准，并更新契约/文档。

## 7. 证据存放

- 原始日志、截图、存档、二进制：`build/runs/`、`local/`、`artifacts/`（均忽略）。
- 可提交的只有：汇总结论、命令、版本、哈希、范围限制、复现步骤。
- 不要把数 MB 日志或游戏派生文件粘进 PR。

## 8. CI

`.github/workflows/host-tests.yml` 在 Ubuntu 上：

1. 安装 `cmake`、`ninja-build`、`gcc`、`python3`、`liblz4-dev`（或 pip lz4）。
2. 配置 `tests/`。
3. 构建并运行全部宿主测试。

CI 不构建 Vita VPK（需要 VitaSDK 和游戏输入），也不运行 Vita3K。

## 9. 本快照验证记录（2026-10-31）

新仓库初始化时执行过：

| 项目 | 结果 | 范围 |
|---|---|---|
| 输入身份 | 通过 | 锁定 1.4.0 ExeFS + NACP；未扫 RomFS |
| VPK 构建 | 通过 | VitaSDK GCC 15.2.0；SELF `d6d210cd…`；VPK 因 zip 时间戳每次不同 |
| 宿主测试（Windows） | 26/26 | MSVC 19.51.36252 / Release |
| POSIX 宿主测试 | 4/4 | WSL Ubuntu 24.04 / GCC 13.3.0，手动编译 |
| Python 语法 | 通过 | `compileall`（缓存写盘受限时用 `PYTHONPYCACHEPREFIX`） |
| 静态包校验 | 通过 | 既有 B118 包：abi=2 stages=2402 variants=364 gxp=364 |
| Vita3K 运行 | 未执行 | 初始化时有既有会话占用 Vita3K；不打断用户运行 |

这些结果只证明对应层级；不代表真机、完整玩法或当前工作树之外的行为。
