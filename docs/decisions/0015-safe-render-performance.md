# 0015 渲染性能安全迁移：只接受两项无语义副作用优化

- 状态：已采纳
- 日期：2026-11-02
- 决策者：项目维护者
- 关联：[渲染性能教训与交接](../13-render-performance-lessons.md)、
  [ADR 0004 GXM 批处理](0004-gxm-batching.md)

## 背景

旧分支 `codex/render-performance`（冻结于 `9f3e810`）完成了村庄 draw census、纹理
工作集、LOD、NVN 接管和缓存容量等大量调查。原始提交 `bcd1d3d` 同时包含多项实验：
CPU/NEON 清屏、pair hint、完成等待、census 字段和诊断路径。整支 merge 或
cherry-pick 会把这些默认关闭但影响资源/时序的实验一并带入，且其中多数没有稳定
整帧收益。

本分支从 `develop` `08d4b6d` 建立，只迁移两项能在不改渲染输出前提下证明正确性的
CPU 侧优化，其余方案保留为历史结论。

## 决定

1. **static pair hint 由 1 项改为 8 项。**
   - hint 只保存 slot 指针、VS/FS token、viewport 和 age；
   - 每次命中仍执行原有 `static_pair_inputs_match` 完整比较（index source、stride、
     color mask、16 个 attribute、6 个 blend 字段）和 `used` 检查；
   - pair 释放/重用前 `static_pair_hint_forget` 清除对应项，reset 清空整表；
   - 不扩大到 32 项；不携带 census、CPU clear 或任何诊断写入。
2. **`gxm_pending_scene_complete` 只等待最新 scene。**
   - 同一 GXM context 的 scene 按通知值顺序提交；需要全部 drain 时等待最新 pending
     scene 的通知值，成功后清理所有更早 pending 并释放其 upload pin；
   - 每个 arena 重用前的 `gxm_pending_scene_complete_arena` 等待保持不变；
   - 等待失败时 poison context，返回原始错误，保留 pending 和全部 pin；
   - 通知值递增统一走 `gxm_notify_next`，跳过 0；最新选择和清理使用同一串行号
     比较（`gxm_notify_newer` / `gxm_notify_reached`），只支持当前最多 3 个
     outstanding scene 的半窗口，防止 32 位回绕提前释放资源。

## 正确性与资源生命周期

- hint 不改变查询结果：完整比较失败就回到原扫描；hint 不持有引用，slot 失效时
  显式清除，避免 stale 指针或旧程序身份。
- scene 通知的顺序假设来自同一 context 的顺序提交；等待最新值成功即证明所有更早
  scene 已完成，满足 staging/upload pin 在复用前必须完成的约束。
- 失败不清理是刻意选择：失败不能证明 GPU 已停止读取资源，因此不能减 pin 或清
  pending；后续由 poison 路径拒绝继续提交。
- 两者都不改变 draw/clear/present 顺序、资源内容、像素或存档语义。

## 风险与限制

- 串行号比较只在 outstanding 值差小于 2^31 时有效；当前每 arena 一个 pending，
  共 3 个，满足条件。若未来增加无界 pending 队列，必须重新设计。
- “等待最新通知证明更早 scene 完成”依赖同一 context 的提交顺序和 GPU 通知语义；
  真机 GXM 行为仍未验证。
- 旧分支的命中率/等待调用下降是 L3、单窗口数据；本分支不宣称 FPS 加速。
- 未迁入 CPU/NEON 清屏、NVN hook、LOD 写入、census、phase-rate/skip/阴影降频、
  direct owned texture bind、缓存扩容、内容 hash、跳过纹理校验、纹理空闲池等；
  这些方案的收益和风险见统一交接文档。

## 回滚

两项都是普通代码改动，没有新增运行时 marker 或持久状态：

- pair hint：恢复单项 hint 结构/函数，或 revert 对应代码；
- 完成等待：恢复逐 arena 等待，或 revert 对应代码。

回滚后无需删除运行目录文件，也不影响已有存档或输入身份。

## 验证

- L1：新增 `tests/gxm_render_pipeline.c`，由 CMake 从生产 `gxm_runtime.c` 抽取实际
  函数体编译；覆盖 hint 多 key/完整匹配拒绝/失效重用、完成等待无 pending/多 arena/
  最新非最大 index/失败不清理不 unpin/成功准确释放 pin/单 arena 等待/回绕与跳过 0。
- L2：输入锁与 VPK 构建通过（本地 GCC 15.2.0）。
- L3/L4：未执行；主控需在 Vita3K/真机复核画面、时序和性能。详见
  [交接文档第 6 节](../13-render-performance-lessons.md#6-本分支验证状态)。
