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

- Windows：46 项 CTest（44 项跨平台，含 4 项 Python 工具回归；另加
  `session_gate`、`runtime_main_window` 两项 Windows 专用）。
- Linux/WSL：额外 4 项 POSIX 测试（`audio_auto_catchup`、`audio_wave_progress`、
  `guest_storage`、`log_sink`）。
- Python 工具回归：`test_movie_timing.py`、`test_movie_color_timing.py`（unittest）。
- GPU 等待：`gpu_wait_profile` 检查生产计数与桶边界，`gpu_wait_metrics` 检查
  完整快照、区间差分、全局计时对账和残缺/倒退数据拒绝。
- 延迟 clear：`clear_defer` 抽取生产准入、提交、物化和消费逻辑，检查先提交后记录、
  drain 失败保留标签、模式切换和原路径；`engine_mode` 检查共享模式控制谓词。
- `static_shader_candidate.c` 是可选集成检查，需要一个候选包路径，不默认加入 CTest。

覆盖范围与证据规则见 [测试](../docs/08-testing.md)。
