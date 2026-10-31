# 0004 GXM 后端与有界 Scene 批处理

- 状态：已采纳
- 日期：2026-10-31
- 决策者：项目维护者

## 背景

原始 NVN 命令流需要映射到 Vita GXM。逐 draw 调用 GXM 的 CPU 开销和同步成本很高；
但无界批处理会破坏资源生命周期、深度/颜色依赖和 staging 预算。

## 决定

1. 使用 GXM 作为唯一目标图形后端；不把 Vulkan 作为验收路径。
2. 在 Maxwell/MME 解码后、GXM 提交前做有界 Scene 批处理：
   - 每 Scene 最多 128 draw、128 KiB uniform；
   - staging 按需分配，每组最多两页 8 MiB，总上限 48 MiB；
   - 资源 pin/fence 生命周期明确，完成前不复用。
3. 颜色 clear 可延迟到下一次同目标 draw（clear fusion），但采样/present/readback/finish
   前必须物化；depth/stencil 顺序保持。
4. 静态 shader/program/纹理描述符有有界缓存；不引入无界常驻。
5. 任何资源失效/目标切换/边界事件都必须 flush 或等待，不能静默重解释。

## 后果

- 减少逐 draw GXM 调用和完成等待，提高村庄帧率。
- 需要严格的 hazard/生命周期检查；错误会导致缺 draw、黑块或闪烁。
- 批处理参数是性能调优重点，但不能以正确性为代价。

## 替代方案

- **逐 draw 提交**：CPU 开销高，已验证性能不足。
- **无界批处理**：内存和生命周期不可控。
- **Vulkan 后端**：不是 Vita 硬件路径，不能作为目标。

## 证据

- 村庄渲染与批处理统计：L3（Vita3K），单次运行范围。
- 真机 GXM 行为：未验证。
