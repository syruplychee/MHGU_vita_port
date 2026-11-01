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

- Windows：28 项（27 项跨平台测试 + `session_gate`）。
- Linux/WSL：额外 4 项 POSIX 测试（`audio_auto_catchup`、`audio_wave_progress`、
  `guest_storage`、`log_sink`）。
- Python 工具回归：`test_movie_timing.py`、`test_movie_color_timing.py`（unittest）。
- `static_shader_candidate.c` 是可选集成检查，需要一个候选包路径，不默认加入 CTest。

覆盖范围与证据规则见 [测试](../docs/08-testing.md)。
