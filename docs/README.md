# 文档索引

本仓库的文档只记录**当前有效的项目共识**。历史逐次运行、原始日志和一次性研究记录
留在旧工作区，不进入本仓库；需要时以旧仓库的 Git 历史为准。

## 按角色阅读

| 角色 | 阅读顺序 |
|---|---|
| 第一次接触项目 | [README](../README.md) → [依赖](02-dependencies.md) → [游戏输入](03-game-inputs.md) → [构建](04-building.md) |
| 负责构建/发布 | [依赖](02-dependencies.md) → [构建](04-building.md) → [运行时资源](05-runtime-assets.md) → [测试](08-testing.md) |
| 图形/性能开发 | [架构](01-architecture.md) → [着色器包](reference/shader-package.md) → [当前状态](09-status.md) → [贡献指南](10-contributing.md) |
| 真机验证 | [硬件](07-hardware.md) → [依赖](02-dependencies.md) → [运行时资源](05-runtime-assets.md) |
| 新贡献者 | [贡献指南](10-contributing.md) → [架构](01-architecture.md) → [测试](08-testing.md) |

## 共识文档

1. [架构与执行模型](01-architecture.md)
2. [依赖、版本与安装](02-dependencies.md)
3. [游戏文件、密钥与输入布局](03-game-inputs.md)
4. [构建 VPK](04-building.md)
5. [运行时资源与静态着色器包](05-runtime-assets.md)
6. [在 Vita3K 中运行](06-running-vita3k.md)
7. [真机与 kubridge](07-hardware.md)
8. [测试、证据分级与验收](08-testing.md)
9. [当前状态与已知问题](09-status.md)
10. [贡献指南](10-contributing.md)
11. [故障排查](11-troubleshooting.md)
12. [develop 开发起点与旧项目交接](12-development-baseline.md)
13. [渲染性能安全迁移：结论、反例与测量教训](13-render-performance-lessons.md)
14. [村庄黑块：原始代码、GXM 与模拟器的证据边界](14-render-black-block-investigation.md)
15. [游戏内部渲染提交：入口线索与下一阶段计划](15-engine-render-submission-plan.md)

## 参考

- [输入身份锁](reference/input-identity.md)
- [运行时数据布局](reference/runtime-data.md)
- [运行时 marker](reference/markers.md)
- [静态着色器包格式](reference/shader-package.md)
- [上游项目与许可](reference/upstreams.md)
- [术语表](glossary.md)

## 架构决策记录（ADR）

`docs/decisions/` 只保留仍然约束当前实现的决策，不复制旧仓库的 100 多份历史记录。
每份 ADR 说明背景、决定、后果和替代方案。

- [0001 保留原始 AArch32 游戏逻辑](decisions/0001-native-execution.md)
- [0002 单一 1.4.0 输入锁](decisions/0002-input-identity-lock.md)
- [0003 离线着色器与静态资源准备](decisions/0003-offline-static-resources.md)
- [0004 GXM 后端与有界 Scene 批处理](decisions/0004-gxm-batching.md)
- [0005 存档真实语义与保护](decisions/0005-save-semantics.md)
- [0006 证据分级](decisions/0006-evidence-levels.md)
- [0007 稳定运行时数据目录](decisions/0007-runtime-data-layout.md)
- [0008 游戏数据不进入 Git](decisions/0008-no-game-data-in-git.md)
- [0009 CPU 视频表面原生读回](decisions/0009-native-video-readback.md)
- [0010 SDK 锁递归与轻量入口](decisions/0010-sdk-lock-bridge.md)
- [0011 视频颜色转换定向计时](decisions/0011-movie-color-conversion-probe.md)
- [0012 条件变量只通知当前等待者](decisions/0012-condition-variable-signal.md)
- [0013 Session 用户态门锁](decisions/0013-session-user-gate.md)
- [0014 框架级原生播片](decisions/0014-native-movie-player.md)
- [0015 渲染性能安全迁移](decisions/0015-safe-render-performance.md)
- [0016 本地 OpenGL 验证后端的顶点点积顺序](decisions/0016-local-vertex-dot-order.md)
- [0017 角色浮点数据纹理保留 32 位精度](decisions/0017-lossless-vertex-float-textures.md)
