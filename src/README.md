# src 目录

| 目录 | 职责 |
|---|---|
| `app/` | 加载器主入口与启动顺序 |
| `loader/` | NSO 解码、重定位、补丁、SDK DSO、启动生命周期 |
| `bridge/` | guest/host ABI 桥、guest TLS、ARMv7 汇编岛 |
| `platform/` | Vita 平台服务、GXM 渲染、音频、视频、存档 |
| `generated/` | 从锁定输入生成的 SDK 契约头（由工具生成，不手改） |
| `bootstrap/` | 无游戏 bootstrap，用于验证 VitaSDK |

生产目标只包含这些目录；研究探针不在本仓库中。

构建与运行说明见 [构建](../docs/04-building.md)。
