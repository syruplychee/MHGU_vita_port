# tools/runtime

Vita3K 启动与证据工具。

| 文件 | 作用 |
|---|---|
| `run_gui_play.ps1` | 主运行器：安装 VPK、staging 数据、启动 Vita3K、截图、退出检查 |
| `prepare_vita3k_runtime.ps1` | 从 `MHGU_VITA3K` / 选择文件/`../vita3k` 解析或建立私有运行时 |
| `renderer_policy.ps1` | 校验 OpenGL 后端配置与初始化日志 |
| `window_capture_verdict.ps1` | 窗口截图有效性判定 |
| `window_close.ps1` | 优雅关闭窗口 |
| `detached_process.ps1` | 分离进程启动/生命周期 |
| `observe_process_lifecycle.ps1` | 进程退出观察 |
| `send_test_input.ps1` | 通过邮箱发送测试输入 |
| `summarize_savedata_io.py` | 从 SDK 日志汇总存档 I/O |

日常入口是 `scripts/play.ps1`，它先备份存档再调用 `run_gui_play.ps1`。
详细参数见 [在 Vita3K 中运行](../../docs/06-running-vita3k.md)。
