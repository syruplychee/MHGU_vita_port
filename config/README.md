# config 目录

本目录只放**构建与运行时契约**，不放历史验证日志、游戏数据或本地路径。

| 路径 | 作用 | 修改要求 |
|---|---|---|
| `target-1.4.0.json` | 唯一输入身份锁 | 需要新版本/新 ADR；不得为适配错误输入而改 |
| `patch-plans/` | SDK DSO/SVC/Timer/Barrier 与 TLS 站点计划 | 由工具从锁定输入生成，审查后提交 |
| `graphics/` | 着色器工具链、输入类型、阴影策略、NVN 槽位 | 工具版本/契约变化时更新 |
| `render-resolution.txt` | `360p` 或 `540p` | 只接受这两个值 |
| `vita3k-play.yml` | Vita3K 基础配置 | 后端必须 OpenGL |
| `reference-lock.json` | 研究/来源记录 | 只读参考，不参与构建 |
| `kubridge-api-lock.json` | 可选真机 API 事实 | 插件不随项目分发 |

本地选择文件 `vita3k-runtime.json` 被 `.gitignore` 忽略；示例见
`vita3k-runtime.example.json`。

## 不要放这里

- 游戏文件、密钥、固件、存档
- 运行日志、截图、artifacts
- 各开发者的绝对路径
- 临时实验配置（放 `local/` 或 `build/`）
