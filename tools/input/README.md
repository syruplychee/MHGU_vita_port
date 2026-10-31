# tools/input

输入身份与构建期契约生成。

| 脚本 | 作用 |
|---|---|
| `verify_input.py` | 校验四模块/NPDM/NACP 大小、SHA256、Build ID、display version |
| `content_manifest.py` | 生成 RomFS 文件清单（本地证据） |
| `inspect_binary.py` | 读取 NSO 段/导入/重定位摘要 |
| `nso_reloc_model.py` | NSO 解码/重定位模型（被 SDK 扫描工具使用） |
| `prepare_native_loader.py` | 生成 `target_identity.h`、`target_tls.h`、`target_startup.h` |
| `prepare_sdk_dso.py` | 校验 SDK DSO 初始化边界契约 |
| `prepare_sdk_svc.py` | 扫描 SDK SVC veneer 并校验/生成契约头 |
| `prepare_sdk_timer.py` | 扫描计时站点并校验/生成契约头 |
| `prepare_sdk_barrier.py` | 扫描 legacy barrier 并校验/生成契约头 |

`prepare_sdk_*` 默认输入为 `../unpacked/update-mhgu/game/0`，在 CI/新布局中请显式传
`--exefs`。非 `--verify` 模式会改写 `config/patch-plans/*`，只在确认输入锁变化时使用。
