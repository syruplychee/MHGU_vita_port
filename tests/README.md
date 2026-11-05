# tests

宿主侧回归测试。它们编译生产源文件/头，不依赖游戏输入、Vita3K 或网络。

```powershell
.\scripts\host-tests.ps1
```

```bash
cmake -S tests -B build/host-tests -DCMAKE_BUILD_TYPE=Release
cmake --build build/host-tests -j
ctest --test-dir build/host-tests --output-on-failure
```

- Windows：50 项 CTest（48 项跨平台，含 7 项 Python 工具回归；另加
  `session_gate`、`runtime_main_window` 两项 Windows 专用）。
- Linux/WSL：额外 4 项 POSIX 测试（`audio_auto_catchup`、`audio_wave_progress`、
  `guest_storage`、`log_sink`）。
- Python 工具回归：`test_movie_timing.py`、`test_movie_color_timing.py`（unittest）。
- GPU 等待：`gpu_wait_profile` 检查生产计数与桶边界，`gpu_wait_metrics` 检查
  完整快照、区间差分、全局计时对账和残缺/倒退数据拒绝。
- 帧成本分解：`frame_cost_profile` 直接编译生产纯逻辑，检查嵌套/递归 self、
  提前返回 wrapper、栈溢出、mismatch/underflow、时钟倒退、关闭时不建 span 和严格
  控制文件解析；`frame_cost_metrics` 检查完整快照行集、同 epoch/帧步进、计数倒退、
  profiler error、负 residual 保留、跨窗口/跨切换拒绝、证据门槛和 mode0 描述性干扰。
  运行分析器：`python tools/engine/analyze_frame_cost.py --gxm-log <log>
  --window <start_us>:<end_us> --output <dir>`。
- 原游戏成本：`game_cost_metrics` 检查父子/worker 重叠不重复计时、边界裁剪、缺失
  journal 拒绝、bridge 不得超出 self，以及有界资源捕获的完整 key 和首尾帧排除。
  `texture_pool` 另验证诊断仍逐字节比较，GPU 持有的旧像素与 token 生命周期不变。
- 延迟 clear：`clear_defer` 抽取生产准入、提交、物化和消费逻辑，检查先提交后记录、
  drain 失败保留标签、模式切换和原路径；`engine_mode` 检查共享模式控制谓词。
- 纹理内存实验：`texture_memory_metrics` 检查同端点实际预算与两类占用对账、跨模式/
  计数重置/缺快照拒绝；`texture_regions`、`texture_pool` 验证共享总额、类隔离及持有期。
- `static_shader_candidate.c` 是可选集成检查，需要一个候选包路径，不默认加入 CTest。

覆盖范围与证据规则见 [测试](../docs/08-testing.md)。
