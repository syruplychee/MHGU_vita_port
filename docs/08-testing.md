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

跨平台测试（26 项）：

- `audio_downmix`、`audio_ipc`
- `batch_policy`
- `clear_fusion`
- `float_bits_numeric`
- `gpu_command_buffer`、`gpu_effective_rt`、`gpu_indices`、`gpu_stream`、
  `gpu_texture_decode`、`gpu_uniform_state`
- `gxm_depth`、`gxm_depth_view`、`gxm_depth_view_shadow`、`gxm_resource_layout`、
  `gxm_sampler_slots`、`gxm_shadow_sampler`
- `memory_attributes`、`nvmap_table`
- `pair_cache_policy`、`render_resolution`
- `static_shader_index`、`texture_snapshot`、`time_calendar`
- `vertex_format`、`vertex_window`

仅 POSIX/Linux（4 项）：

- `audio_auto_catchup`（`sys/mman.h`、`MAP_32BIT`）
- `audio_wave_progress`（`MAP_32BIT`）
- `guest_storage`（32 位 guest 地址 + POSIX mmap）
- `log_sink`（pthread）

这些测试直接编译生产源文件或包含生产实现，不是复制品。

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
